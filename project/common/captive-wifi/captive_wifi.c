#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "captive_wifi.h"

static const char *TAG = "captive_wifi";

#define NS          "cwifi"
#define KEY_SSID    "ssid"
#define KEY_PASS    "pass"

#define PORTAL_URI          "http://192.168.4.1/"
#define AP_MAX_CLIENTS      4
#define SCAN_MAX            20
#define CONNECT_TIMEOUT_MS  30000   /* portal: give up on a connection attempt */
#define CONNECT_TRIES       3       /* portal: disconnects before giving up */
#define STOP_DELAY_MS       10000   /* portal: keep the page up after success */
#define WORKER_PERIOD_MS    500

/* ------------------------------------------------------------------ state */

typedef enum {
    CMD_PORTAL_START,
    CMD_PORTAL_STOP,
} cmd_t;

typedef enum {
    TRY_IDLE,
    TRY_CONNECTING,
    TRY_OK,
    TRY_FAILED,
} try_state_t;

static captive_wifi_cb_t s_cb;
static void *s_cb_ctx;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static QueueHandle_t s_cmd;
static char s_ap_ssid[32];

static volatile bool s_connected;      /* STA has an IP and the app was told */
static volatile bool s_got_ip;         /* STA has an IP */
static volatile bool s_ever_connected; /* since captive_wifi_start() */
static volatile bool s_portal;
static volatile bool s_wifi_started;
static int  s_fail_count;

/* portal connection attempt, shown on the page through /status */
static volatile try_state_t s_try;
static volatile int s_try_disconnects;
static volatile int s_try_reason;
static TickType_t s_try_started;
static TickType_t s_stop_at;           /* 0 = no stop pending */
static TickType_t s_portal_started;
static char s_try_ssid[33];
static char s_try_pass[65];

static httpd_handle_t s_httpd;
static volatile bool s_dns_run;
static TaskHandle_t s_dns_task;

static void emit(captive_wifi_event_t evt)
{
    if (s_cb) {
        s_cb(evt, s_cb_ctx);
    }
}

/* ------------------------------------------------------------ credentials */

static esp_err_t creds_load(wifi_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t l1 = sizeof(cfg->sta.ssid), l2 = sizeof(cfg->sta.password);
    err = nvs_get_str(h, KEY_SSID, (char *)cfg->sta.ssid, &l1);
    if (err == ESP_OK) {
        err = nvs_get_str(h, KEY_PASS, (char *)cfg->sta.password, &l2);
        if (err == ESP_ERR_NVS_NOT_FOUND) {      /* open network */
            cfg->sta.password[0] = 0;
            err = ESP_OK;
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = pass[0] ? nvs_set_str(h, KEY_PASS, pass) : nvs_erase_key(h, KEY_PASS);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool captive_wifi_has_credentials(void)
{
    wifi_config_t cfg = {0};
    return creds_load(&cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

esp_err_t captive_wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "credentials cleared");
    return err;
}

static void sta_set_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* -------------------------------------------------------------------- DNS */

/* Answers every A query with the SoftAP address, so any name the phone looks
 * up leads to the portal. Other query types get an empty NOERROR answer. */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        goto out;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        goto out;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip);

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;                       /* timeout or runt packet */
        }
        if ((buf[2] & 0x80) || (buf[2] & 0x78) || buf[4] != 0 || buf[5] == 0) {
            continue;                       /* not a standard query */
        }
        /* walk the first question name */
        int p = 12;
        while (p < len && buf[p] != 0) {
            if (buf[p] & 0xC0) {
                p = len;                    /* no compression in questions */
                break;
            }
            p += buf[p] + 1;
        }
        if (p + 5 > len) {
            continue;
        }
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int qend = p + 5;                   /* zero byte + QTYPE + QCLASS */

        buf[2] = 0x84 | (buf[2] & 0x01);    /* QR, AA, keep RD */
        buf[3] = 0x00;                      /* RA = 0, RCODE = NOERROR */
        buf[4] = 0; buf[5] = 1;             /* one question */
        buf[8] = 0; buf[9] = 0;             /* no authority records */
        buf[10] = 0; buf[11] = 0;           /* no additional records (drops EDNS) */
        int out = qend;
        if (qtype == 1 /* A */ || qtype == 255 /* ANY */) {
            buf[6] = 0; buf[7] = 1;
            const uint8_t ans[] = {
                0xC0, 0x0C,                 /* name: pointer to the question */
                0x00, 0x01, 0x00, 0x01,     /* type A, class IN */
                0x00, 0x00, 0x00, 0x3C,     /* TTL 60 s */
                0x00, 0x04,
            };
            memcpy(&buf[out], ans, sizeof(ans));
            out += sizeof(ans);
            memcpy(&buf[out], &ip.ip.addr, 4);
            out += 4;
        } else {
            buf[6] = 0; buf[7] = 0;
        }
        sendto(sock, buf, out, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
out:
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    s_dns_run = true;
    if (xTaskCreate(dns_task, "cwifi_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        ESP_LOGE(TAG, "DNS task not created");
    }
}

static void dns_stop(void)
{
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) {   /* recv timeout is 1 s */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------------- HTTP */

static const char PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Wi-Fi setup</title><style>"
"body{font-family:sans-serif;margin:0;padding:16px;max-width:420px;background:#f4f4f4;color:#222}"
"h1{font-size:20px;margin:0 0 4px}p{margin:4px 0 12px;color:#555}"
"#list div{background:#fff;padding:12px;margin:6px 0;border-radius:6px;cursor:pointer;"
"display:flex;justify-content:space-between}#list div.sel{outline:2px solid #2a7}"
"input,button{width:100%;box-sizing:border-box;font-size:16px;padding:10px;margin:6px 0;"
"border-radius:6px;border:1px solid #bbb}button{background:#2a7;color:#fff;border:0}"
"button.sec{background:#888}#st{margin-top:12px;font-weight:bold}"
"</style></head><body>"
"<h1 id='dev'>Wi-Fi setup</h1><p>Choose the network for this device.</p>"
"<button class='sec' onclick='scan()'>Scan again</button><div id='list'>Scanning...</div>"
"<input id='ssid' placeholder='Network name' maxlength='32'>"
"<input id='pass' type='password' placeholder='Password' maxlength='64'>"
"<label><input type='checkbox' style='width:auto' "
"onclick=\"pass.type=this.checked?'text':'password'\"> show password</label>"
"<button onclick='conn()'>Connect</button><div id='st'></div>"
"<script>"
"function esc(s){return s.replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
"function bars(r){return r>-55?'||||':r>-67?'|||':r>-78?'||':'|'}"
"function scan(){list.textContent='Scanning...';fetch('/scan').then(r=>r.json()).then(a=>{"
"list.innerHTML='';if(!a.length)list.textContent='No networks found';"
"a.forEach(n=>{let d=document.createElement('div');"
"d.innerHTML='<span>'+esc(n.ssid)+(n.auth?' &#128274;':'')+'</span><span>'+bars(n.rssi)+'</span>';"
"d.onclick=()=>{document.querySelectorAll('#list div').forEach(x=>x.className='');"
"d.className='sel';ssid.value=n.ssid;pass.focus()};list.appendChild(d)})})"
".catch(()=>list.textContent='Scan failed')}"
"function conn(){if(!ssid.value){st.textContent='Choose a network';return}"
"st.textContent='Connecting...';let b='ssid='+encodeURIComponent(ssid.value)+"
"'&pass='+encodeURIComponent(pass.value);"
"fetch('/connect',{method:'POST',body:b}).then(r=>r.json()).then(j=>{"
"if(j.ok)setTimeout(poll,1500);else st.textContent=j.msg}).catch(()=>setTimeout(poll,1500))}"
"function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
"if(j.state=='connecting'){st.textContent='Connecting...';setTimeout(poll,1500)}"
"else if(j.state=='ok')st.textContent='Connected, IP '+j.ip+'. The device leaves setup mode now.';"
"else if(j.state=='failed')st.textContent='Not connected: '+j.msg;"
"}).catch(()=>{st.textContent='Connecting... (the phone may drop this network for a moment)';"
"setTimeout(poll,2000)})}"
"fetch('/status').then(r=>r.json()).then(j=>{dev.textContent=j.device+' Wi-Fi setup'});scan()"
"</script></body></html>";

static void json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            o += snprintf(&out[o], len - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    out[o] = 0;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static const char *reason_text(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password?";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "unsupported security";
    case 0:
        return "no answer from the network";
    default:
        return "connection failed";
    }
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (s_try == TRY_CONNECTING || esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = SCAN_MAX;
    wifi_ap_record_t *rec = calloc(SCAN_MAX, sizeof(wifi_ap_record_t));
    if (!rec) {
        esp_wifi_clear_ap_list();
        return httpd_resp_send_500(req);
    }
    esp_wifi_scan_get_ap_records(&n, rec);   /* sorted by signal, strongest first */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (rec[i].ssid[0] == 0) {
            continue;
        }
        bool dup = false;                    /* same SSID from several APs */
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp((char *)rec[j].ssid, (char *)rec[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        char ssid[200], item[260];
        json_str(ssid, sizeof(ssid), (char *)rec[i].ssid);
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 first ? "" : ",", ssid, rec[i].rssi, rec[i].authmode != WIFI_AUTH_OPEN);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    free(rec);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[320];             /* both fields fully percent-encoded */
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"bad request\"}");
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[100] = "", pass[200] = "";
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl == 0 || sl > 32) {
        return send_json(req, "{\"ok\":false,\"msg\":\"network name must be 1-32 characters\"}");
    }
    if (pl != 0 && (pl < 8 || pl > 63)) {
        return send_json(req, "{\"ok\":false,\"msg\":\"password must be 8-63 characters\"}");
    }
    if (s_try == TRY_CONNECTING) {
        return send_json(req, "{\"ok\":false,\"msg\":\"already connecting\"}");
    }

    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    strlcpy(s_try_pass, pass, sizeof(s_try_pass));
    s_try_disconnects = 0;
    s_try_reason = 0;
    s_try_started = xTaskGetTickCount();
    s_try = TRY_CONNECTING;
    ESP_LOGI(TAG, "portal: connecting to %s", s_try_ssid);

    send_json(req, "{\"ok\":true}");        /* answer before the radio may change channel */
    if (s_got_ip) {
        esp_wifi_disconnect();
    }
    sta_set_config(s_try_ssid, s_try_pass);
    esp_wifi_connect();
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char dev[80], json[200];
    json_str(dev, sizeof(dev), s_ap_ssid);
    switch (s_try) {
    case TRY_CONNECTING:
        snprintf(json, sizeof(json), "{\"device\":\"%s\",\"state\":\"connecting\"}", dev);
        break;
    case TRY_OK: {
        char ip[16] = "";
        captive_wifi_get_ip(ip, sizeof(ip));
        snprintf(json, sizeof(json), "{\"device\":\"%s\",\"state\":\"ok\",\"ip\":\"%s\"}", dev, ip);
        break;
    }
    case TRY_FAILED:
        snprintf(json, sizeof(json), "{\"device\":\"%s\",\"state\":\"failed\",\"msg\":\"%s\"}",
                 dev, reason_text(s_try_reason));
        break;
    default:
        snprintf(json, sizeof(json), "{\"device\":\"%s\",\"state\":\"idle\"}", dev);
        break;
    }
    return send_json(req, json);
}

/* Any other URL (the phone's connectivity checks included) is sent to the
 * portal page; iOS needs a body in the response to show the sign-in sheet. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_URI);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Redirect to the Wi-Fi setup page");
}

static esp_err_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = page_get },
        { .uri = "/scan",    .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/status",  .method = HTTP_GET,  .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    /* redirected probes of the phones would otherwise flood the log */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    return ESP_OK;
}

/* ----------------------------------------------------------------- portal */

static void portal_start(void)
{
    if (s_portal) {
        return;
    }
    ESP_LOGI(TAG, "starting setup portal, AP %s", s_ap_ssid);
    s_portal = true;                        /* stops STA auto-reconnect */
    s_try = TRY_IDLE;
    s_stop_at = 0;
    s_portal_started = xTaskGetTickCount();
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = AP_MAX_CLIENTS;
    ap.ap.csa_count = 3;
    const char *pw = CONFIG_CAPTIVE_WIFI_AP_PASSWORD;
    if (strlen(pw) >= 8) {
        strlcpy((char *)ap.ap.password, pw, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (pw[0]) {
            ESP_LOGW(TAG, "AP password shorter than 8 characters, AP stays open");
        }
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);     /* STA is needed for scanning */
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        }
        s_wifi_started = true;
    }

    /* DHCP option 114: the sign-in page address (the pointer is kept) */
    static char uri[] = PORTAL_URI;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(s_ap_netif);
    dns_start();
    http_start();
    emit(CAPTIVE_WIFI_EVT_PORTAL_STARTED);
}

/* Portal down, STA only. The app hears PORTAL_STOPPED first and STA_CONNECTED
 * after it, so whatever it starts on the connection (HomeKit on port 80) never
 * meets the portal's web server. */
static void portal_stop(void)
{
    if (!s_portal) {
        return;
    }
    ESP_LOGI(TAG, "stopping setup portal");
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    dns_stop();
    if (s_wifi_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    s_portal = false;
    s_try = TRY_IDLE;
    s_stop_at = 0;
    emit(CAPTIVE_WIFI_EVT_PORTAL_STOPPED);
    if (s_got_ip && !s_connected) {
        s_connected = true;
        emit(CAPTIVE_WIFI_EVT_STA_CONNECTED);
    }
}

/* Runs the portal start/stop and its timeouts outside the event loop task,
 * whose stack is too small for the web server calls. */
static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t cmd;
        if (xQueueReceive(s_cmd, &cmd, pdMS_TO_TICKS(WORKER_PERIOD_MS)) == pdTRUE) {
            if (cmd == CMD_PORTAL_START) {
                portal_start();
            } else if (cmd == CMD_PORTAL_STOP) {
                portal_stop();
            }
            continue;
        }
        if (!s_portal) {
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        if (s_try == TRY_CONNECTING &&
            now - s_try_started > pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "portal: no connection to %s", s_try_ssid);
            esp_wifi_disconnect();
            s_try = TRY_FAILED;
            emit(CAPTIVE_WIFI_EVT_PORTAL_CONNECT_FAILED);
        }
        if (s_stop_at && (int32_t)(now - s_stop_at) >= 0) {
            portal_stop();
            continue;
        }
#if CONFIG_CAPTIVE_WIFI_PORTAL_TIMEOUT_MIN > 0
        /* stored network unreachable for a while: give it another chance */
        if (s_try != TRY_CONNECTING && s_try != TRY_OK && captive_wifi_has_credentials() &&
            now - s_portal_started > pdMS_TO_TICKS(CONFIG_CAPTIVE_WIFI_PORTAL_TIMEOUT_MIN * 60000)) {
            ESP_LOGI(TAG, "portal timeout, retrying the stored network");
            portal_stop();
            wifi_config_t cfg = {0};
            if (creds_load(&cfg) == ESP_OK) {
                s_fail_count = 0;
                sta_set_config((char *)cfg.sta.ssid, (char *)cfg.sta.password);
                esp_wifi_connect();
            }
        }
#endif
    }
}

static void post(cmd_t cmd)
{
    if (s_cmd) {
        xQueueSend(s_cmd, &cmd, 0);
    }
}

/* ----------------------------------------------------------------- events */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_portal) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        esp_netif_create_ip6_linklocal(s_sta_netif);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        s_ever_connected = true;
        s_fail_count = 0;
        if (s_portal) {
            if (s_try == TRY_CONNECTING) {
                if (creds_save(s_try_ssid, s_try_pass) != ESP_OK) {
                    ESP_LOGE(TAG, "credentials not saved");
                }
                s_try = TRY_OK;
                s_stop_at = xTaskGetTickCount() + pdMS_TO_TICKS(STOP_DELAY_MS);
            }
            /* STA_CONNECTED is sent once the portal is down */
        } else if (!s_connected) {
            s_connected = true;
            emit(CAPTIVE_WIFI_EVT_STA_CONNECTED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_got_ip = false;
        if (s_connected) {
            s_connected = false;
            emit(CAPTIVE_WIFI_EVT_STA_DISCONNECTED);
        }
        if (s_portal) {
            /* only the attempt started from the page is retried */
            if (s_try == TRY_CONNECTING && e->reason != WIFI_REASON_ASSOC_LEAVE) {
                s_try_reason = e->reason;
                if (++s_try_disconnects >= CONNECT_TRIES) {
                    ESP_LOGW(TAG, "portal: %s failed, reason %d", s_try_ssid, e->reason);
                    s_try = TRY_FAILED;
                    emit(CAPTIVE_WIFI_EVT_PORTAL_CONNECT_FAILED);
                } else {
                    esp_wifi_connect();
                }
            }
            return;
        }
        s_fail_count++;
        ESP_LOGI(TAG, "disconnected (%d, reason %d), reconnecting", s_fail_count, e->reason);
        if (!s_ever_connected && s_fail_count >= CONFIG_CAPTIVE_WIFI_MAX_RETRIES) {
            /* the stored network never answered since boot: let the user fix it */
            post(CMD_PORTAL_START);
            return;
        }
        esp_wifi_connect();
    }
}

/* -------------------------------------------------------------------- API */

esp_err_t captive_wifi_init(const char *hostname, captive_wifi_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (hostname) {
        esp_netif_set_hostname(s_sta_netif, hostname);
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MPPT-%02X%02X%02X", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  /* credentials live in our own NVS keys */

    s_cmd = xQueueCreate(4, sizeof(cmd_t));
    if (!s_cmd || xTaskCreate(worker_task, "cwifi", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t captive_wifi_start(void)
{
    wifi_config_t cfg = {0};
    s_fail_count = 0;
    s_ever_connected = false;
    if (creds_load(&cfg) != ESP_OK || cfg.sta.ssid[0] == 0) {
        ESP_LOGI(TAG, "no stored network");
        post(CMD_PORTAL_START);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "connecting to %s", cfg.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    sta_set_config((char *)cfg.sta.ssid, (char *)cfg.sta.password);
    esp_err_t err = esp_wifi_start();
    s_wifi_started = (err == ESP_OK);
    return err;
}

esp_err_t captive_wifi_stop(void)
{
    portal_stop();
    esp_err_t err = esp_wifi_stop();
    s_wifi_started = false;
    s_got_ip = false;
    if (s_connected) {
        s_connected = false;
        emit(CAPTIVE_WIFI_EVT_STA_DISCONNECTED);
    }
    return err;
}

bool captive_wifi_is_connected(void)
{
    return s_connected;
}

bool captive_wifi_portal_active(void)
{
    return s_portal;
}

const char *captive_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t captive_wifi_get_ip(char *buf, size_t len)
{
    if (!s_got_ip || !s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip;
    esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip);
    if (err == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
    return err;
}

const char *captive_wifi_portal_ip(void)
{
    return "192.168.4.1";
}

int captive_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_got_ip || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
