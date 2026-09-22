# mppt-1k

Open-source 1 kW MPPT solar charge controller firmware for the **ESP32-S2**, written in C on
**ESP-IDF v5.5.4**, with optional Apple **HomeKit** (HAP) support.

The charging logic is a port of Angelo Casimiro's
[FUGU-ARDUINO-MPPT-FIRMWARE](https://github.com/AngeloCasi/FUGU-ARDUINO-MPPT-FIRMWARE)
(V1.10, CC0 1.0 / public domain) from Arduino/ESP32 to ESP-IDF/ESP32-S2, running on the
**MPPT32 v1.1** board (`files/Schematic_MPPT32_2026-09-22.pdf`).

> **Status: design phase.** This document is the agreed specification. No application code
> has been written yet; `project/mppt-hap/main/app_main.c` is a HomeKit template that will be
> replaced.

---

## 1. Hardware: MPPT32 v1.1

Synchronous buck MPPT charger, same electrical topology and sensing chain as FUGU.

| Function | Part | Notes |
|---|---|---|
| MCU | ESP32-S2-SOLO-2-N4 | single core, Wi-Fi, native USB, no Bluetooth |
| ADC | ADS1115 (16-bit, I2C, addr 0x48) | Vin, Vout, current, temperature |
| Current sensor | ACS712ELCTR-30A | 66 mV/A, 5 V supply |
| Gate driver | IR2104S | half-bridge, SD# = enable |
| Power MOSFETs | CSD19505 ×3 (+ ×3 parallel footprints) | Q1/Q1A backflow, Q2/Q2A high side, Q3/Q3A low side |
| Backflow gate supply | B1212S-1W isolated DC/DC | switched by Q4 (SI2306A) |
| 12 V rail | LM5164 buck from PV input | powers gate driver, fan, 7805 |
| 5 V / 3.3 V | LM7805 → LD1117V33 | 5 V for ACS712 and LCD; USB 5 V also feeds the 3.3 V LDO |
| Inductor | L8, 60 µH (schematic) | parameters to be measured |
| Fan | 3-wire 12 V fan on T3 (pin 2 = 12 V, pin 3 = switched GND via Q5 SI2306A, pin 1 NC) | on/off only; tachometer wire unconnected, no speed control |
| Display | 16×2 HD44780 LCD with PCF8574 I2C backpack, T4 connector (5 V) | |
| Buttons | UP, DOWN, MENU, T5 connector, active low, no external pull-ups | internal pull-ups used |
| Thermistors | TH1, TH2, 10 kΩ NTC, on PCB between the TO-220 MOSFETs | |
| Protection | 30 A fuses F1/F2, VDR R1, TVS on USB | |

### 1.1 GPIO map (ESP32-S2)

| Net | GPIO | Direction | Function |
|---|---|---|---|
| PWM | IO13 | out (LEDC) | IR2104 IN, buck PWM |
| SDWN | IO38 | out | IR2104 SD#, buck enable (high = enabled, R13 pull-down) |
| BFLOW | IO12 | out | backflow MOSFET enable (high = B1212 on = Q1/Q1A on, R20 pull-down → off at reset) |
| FAN | IO3 | out | fan (high = on) |
| PWR12 | IO4 | in | LM5164 PGOOD (open drain, R26 10 kΩ pull-up): high = 12 V rail OK. EN/UVLO is tied to VIN, the rail is always enabled |
| SDA1 / SCL1 | IO35 / IO36 | I2C bus 0 | ADS1115 (10 kΩ pull-ups to 3.3 V) |
| SDA2 / SCL2 | IO9 / IO10 | I2C bus 1 | LCD PCF8574 (10 kΩ pull-ups to 3.3 V, LCD powered from 5 V) |
| READ | IO39 | in | ADS1115 ALERT/RDY (10 kΩ pull-up) |
| ADC0 | IO1 | ADC1_CH0 | TH1 (also wired to ADS1115 AIN1 through R27 0 Ω) |
| ADC1 | IO14 | ADC2_CH3 | TH2 (ADC2 is not usable while Wi-Fi is active on ESP32-S2) |
| DWN / UP / MENU | IO5 / IO6 / IO7 | in, pull-up | buttons |
| IO0 | IO0 | in | boot button S2A; HomeKit reset button (3 s Wi-Fi reset, 10 s factory reset) |
| TXD0 / RXD0 | | UART0 | console / telemetry, T6 header |
| IO19 / IO20 | | USB D- / D+ | native USB (console optional) |

There is no status LED on the MPPT32 v1.1 board (D3 is a hardware indicator on the backflow
gate supply). The firmware drives the `led_indicator` engine from `outputwrite` anyway (Wi-Fi
state as steady base, HAP identify, OTA blink, reset patterns, over-temperature); the GPIO is
`CONFIG_LED_GPIO` and defaults to IO2, which is unconnected on the module, so an external LED
can be wired to it. GPIO configuration goes through `outputwrite` (`ioInit`, `OutputWrite`,
`ReadInput`).

### 1.2 Analog channels (ADS1115)

| Channel | Net | Circuit | Scale | FUGU equivalent |
|---|---|---|---|---|
| AIN0 | A0 | Vout: (R16 ‖ R18 = 23.5 kΩ) / R17 1 kΩ | ×24.5 | `outVoltageDivRatio = 24.5` (FUGU uses channel 1) |
| AIN1 | A1 | TH1 NTC divider (3V3 → NTC → node → 10 kΩ → GND) | NTC, mode "NTC to VCC" | FUGU uses on-chip ADC |
| AIN2 | A2 | ACS712 VIOUT through R4 3.3 kΩ / R5 10 kΩ | ×1.33, midpoint ≈ 2.5 V, 66 mV/A | `*1.3300`, `currentSensV = 0.066` |
| AIN3 | A3 | Vin: R2 200 kΩ / R3 5.1 kΩ | ×40.216 | `inVoltageDivRatio = 40.2156` |

FUGU calibration constants transfer unchanged. ADS1115 is run single-shot at 860 SPS; one
sensor cycle (3+3+4 conversions as in FUGU) takes about 15 ms.

### 1.3 Thermistors

Both TH1 and TH2 are 3V3 → NTC → node → 10 kΩ → GND (`CIRCUIT_MODE_NTC_VCC` in
`espressif/ntc_driver` terms). Default parameters until calibrated by measurement:
R25 = 10 kΩ, B = 3950, R_fixed = 10 kΩ, Vdd = 3300 mV.

- **TH1 is the primary sensor**, read through ADS1115 AIN1 (16-bit, independent of Wi-Fi).
- **TH2 is redundant**, read through IO14 (ADC2) with `ntc_driver`, only while Wi-Fi is off.

### 1.4 Power for bench testing

USB 5 V feeds the 3.3 V LDO, so the MCU, ADS1115 and LCD run from USB alone without PV input
and without the power stage populated. FUGU's `inputSource = 0` state covers this. Phases 1
and 2 (below) can therefore be tested on a board with only the MCU, ADS1115, dividers,
thermistors, LCD and buttons fitted.

### 1.5 Pending hardware data

- L8: inductance, DC resistance, wire, turns, core dimensions (to be measured). Core material
  is unknown; saturation will be checked indirectly from ripple current under load.
- NTC B constant: to be determined from a two-temperature measurement.

---

## 2. Firmware architecture

### 2.1 Toolchain and dependencies

- ESP-IDF **v5.5.4**, target `esp32s2`.
- New I2C driver (`driver/i2c_master`) for **all** I2C devices. The legacy `driver/i2c.h` and
  `i2c_master` cannot coexist in one firmware.
- Components in `project/common` (this repository):
  - `esp32-ads1115` (Molorius) — will be ported to `i2c_master`, `QueueHandle_t`, and given a
    `CMakeLists.txt`.
  - `container_nvs` — blob storage wrapper over `nvs_flash`, used for all settings.
  - `captive-wifi` (new, MIT) — Wi-Fi station management with captive-portal fallback,
    replaces `app_wifi` from the HomeKit SDK. See 2.6.
- Managed components (`main/idf_component.yml`):
  - `espressif/ntc_driver` — NTC on the on-chip ADC (TH2).
  - `esp-idf-lib/hd44780` — HD44780 driver used standalone with a project-supplied PCF8574
    write callback over `i2c_master` (no `i2cdev`/`pcf8574` dependencies).
- External components expected in the build tree but **not** part of this repository
  (`components/*.txt` point to them): `esp-homekit-sdk` (`homekit`), `button` (`iot_button`),
  `outputwrite`, `fupdateota`, `app_hap_setup_payload`, `qrcode`, `mdns` (registry; required
  by HomeKit and reused by `captive-wifi`).

### 2.2 Source layout (`project/mppt-hap/main`)

| File | Responsibility | FUGU origin |
|---|---|---|
| `app_main.c` | boot, safe outputs first, NVS, tasks, Wi-Fi / HAP start if enabled, IO0 reset button, OTA | `setup()` |
| `mppt_config.h` | pins and constants from Kconfig | `#define`s |
| `mppt_state.h` | shared measurement/state struct + mutex | globals |
| `mppt_hal.c/.h` | I2C buses, ADS1115, LCD callback, LEDC PWM, GPIO, NTC ADC | Arduino calls |
| `mppt_sensors.c/.h` | Read_Sensors, current-sensor auto-zero, power, SOC, Wh | `2_Read_Sensors.ino` |
| `mppt_control.c/.h` | Device_Protection, backflowControl, Charging_Algorithm, System_Processes | tabs 3, 4, 5 |
| `mppt_settings.c/.h` | load/save/factory defaults in NVS | `5_System_Processes.ino` EEPROM part |
| `mppt_lcd.c/.h` | display pages and 3-button menu | `8_LCD_Menu.ino` |
| `mppt_telemetry.c/.h` | periodic log line (`ESP_LOGI`) | `6_Onboard_Telemetry.ino` |
| `mppt_hap.c/.h` | HomeKit services, started only when enabled | replaces `7_Wireless_Telemetry.ino` (Blynk) |
| `project/common/captive-wifi` | Wi-Fi STA + captive portal component (2.6) | `setupWiFi()` |

### 2.3 Tasks (single core)

| Task | Priority | Period | Work |
|---|---|---|---|
| `mppt_task` | high (5) | bound by ADS1115 conversions, ~15 ms | Read_Sensors → Device_Protection → System_Processes → Charging_Algorithm |
| `ui_task` | low (1) | 1 s LCD, log at configurable interval | LCD pages, menu state machine, telemetry line, HAP characteristic push (2 s, on change above thresholds) |
| HAP / Wi-Fi tasks | SDK | | only when HAP is enabled |
| buttons | `iot_button` callbacks | | short / long press events into `ui_task` queue |

Shared state (`mppt_state_t`) and settings (`mppt_settings_t`) are mutex-protected. HAP and
menu writes go to settings; `mppt_task` reads them at the start of each cycle.

### 2.4 Start-up safety

The first statements of `app_main()` drive SDWN = 0, PWM duty = 0 and BFLOW = 0 (both already
pulled down by hardware) before NVS, Wi-Fi and HAP initialisation, which take several seconds.

### 2.5 Behaviour ported from FUGU

Kept 1:1: sensor averaging and scaling, automatic current-sensor midpoint calibration,
power-source detection, protection flags (OTE, IOC, OOC, OOV, FLV, IUV, BNC, REC), backflow
control, predictive PWM, P&O MPPT with CC-CV, PSU mode, fan control, Wh/kWh accounting,
4 display pages and the 12 settings sub-menus (re-mapped to 3 buttons).

Changed or fixed:

1. Error-counter reset in `Device_Protection` compared a never-updated timestamp
   (`currentErrorMillis`) and therefore never ran. Fixed.
2. `backflowControl()` was called twice per cycle in charger mode. Called once.
3. Temperature comes from ADS1115 (TH1) via `ntc_driver`-style Beta formula instead of the
   12-bit raw-count Steinhart-Hart expression.
4. Settings are stored as typed values in NVS (namespace `mppt`) instead of whole/hundredths
   byte pairs in EEPROM.
5. Wh counters are persisted to NVS every 15 min or every 10 Wh, whichever comes first.
6. The P&O step (±1 LSB per cycle in FUGU) becomes a Kconfig parameter, default 1.
7. Battery presets replace free-form voltages as the primary way to configure the battery
   (see 3.2). Free-form voltage/current editing remains available.

Kept with changes: `electricalPrice` / `energySavings` stay (default 0.27 EUR/kWh), but the
price becomes a menu setting and the savings are shown on the LCD only, not in HomeKit.

Dropped: Blynk telemetry, Bluetooth flag, dual-core task pinning, the Arduino `String`
firmware info strings (replaced by `CONFIG_APP_PROJECT_VER`), the Light Sensor service of the
HomeKit template.

New relative to FUGU: 12 V rail power-good input (PWR12) checked before enabling the gate
driver, HomeKit, Wi-Fi captive portal, single-firmware Wi-Fi / HAP on/off from the menu, OTA
update via `fupdateota` (automatic check 1 min after Wi-Fi connects, and on demand from the
menu).

### 2.6 `captive-wifi` component

Own component in `project/common/captive-wifi`, MIT, written against ESP-IDF 5.5 APIs. It
follows the idea of tonyp7/esp32-wifi-manager and the structure of the official
`examples/protocols/http_server/captive_portal` example; it keeps the `app_wifi` interface
that the HomeKit code already uses (`app_wifi_init`, `app_wifi_start`, `TakeStatusConnected`
callback) so `mppt_hap` needs no changes. Target size: 500–600 lines of C plus ~6 KB embedded
HTML, no dependencies beyond `esp_wifi`, `esp_netif`, `esp_http_server`, `lwip`, `nvs_flash`
and `mdns`.

Behaviour:

1. Start (only when the Wi-Fi setting is on): if credentials exist in NVS → STA, connect with
   retries; on success → mDNS, HAP (if enabled), `TakeStatusConnected(true)`.
2. No credentials, or N failed attempts → open SoftAP `MPPT-xxxxxx` (private project, at most
   a few devices; a password can be set in Kconfig) with captive portal: DNS catch-all + DHCP option 114, so the sign-in page
   opens automatically on iOS, Android and Windows.
3. Portal page: scanned networks with signal strength, password field, connect button,
   status. HTTP endpoints `/`, `/scan`, `/connect`, `/status`.
4. Credentials saved through `container_nvs`; the device switches to STA (reboot only if
   needed).
5. LCD shows the AP name and `192.168.4.1` while the portal is active, and IP + RSSI when
   connected.
6. "Reset WiFi" (menu, or IO0 held 3 s) erases credentials and returns to the portal.

HTTP server and port 80: the HomeKit SDK runs its own `esp_http_server` instance on port 80
(`hap_platform_httpd`, stack 12 KB, 8 sockets, 16 URI handlers by default, of which HAP uses 8:
`/pair-setup`, `/pair-verify`, `/accessories`, `/characteristics` GET+PUT, `/pairings`,
`/identify`, `/prepare`). Registering application pages on that instance is the supported way
(the original `app_wifi` does it via `hap_platform_httpd_get_handle()`): HAP encrypts only its
own controller sessions through per-socket send/receive overrides after pair-verify, browser
sessions stay plain HTTP and unknown URIs get a 404 from the same server. `captive-wifi`
therefore runs its own server only while the portal is active (AP mode, HAP not yet started)
and stops it before `hap_start()`. The later status web page (phase 5) registers its URI
handlers on the HAP server when HAP is on (keeping the page small: static HTML plus a JSON
endpoint polled every few seconds, so it never competes with pairing crypto for long), and on
a `captive-wifi` server when HAP is off. Both never listen on port 80 at the same time. Wi-Fi is
a setting separate from HAP for this status page.

`app_wifi` facts carried over: `esp_netif_init`, default event loop, STA netif with hostname,
`WIFI_INIT_CONFIG_DEFAULT`, reconnect on `WIFI_EVENT_STA_DISCONNECTED`, IPv6 link-local,
`TakeStatusConnected(true/false)` on got-IP / disconnected. Dropped: `wifi_provisioning`
(deprecated in IDF 5.x), QR code, WAC, hard-coded credentials.

---

## 3. Settings

### 3.1 Runtime settings (NVS, editable from LCD menu and, where noted, HomeKit)

| Setting | Default | Range / values | Source |
|---|---|---|---|
| Charging enabled | on | on/off | menu, HomeKit Switch |
| Output mode | Charger | Charger / PSU | menu, HomeKit custom |
| MPPT algorithm | on | on = MPPT, off = CC-CV only | menu, HomeKit custom |
| Battery preset | None | see 3.2; the user selects the type after first power-up | menu |
| Battery max voltage | 27.30 V (FUGU) | 0–50 V, 0.01 V step | menu, HomeKit custom |
| Battery min voltage | 22.40 V (FUGU) | 0–50 V | menu, HomeKit custom |
| Charging current | 30.0 A | 0–30 A | menu, HomeKit custom |
| Fan enabled | on | on/off | menu, HomeKit Fan |
| Fan on temperature | 60 °C | 0–100 °C | menu |
| Shutdown temperature | 90 °C | 0–120 °C | menu |
| Wi-Fi enabled | off | on/off; hidden in the menu while HAP is on (HAP keeps Wi-Fi on) | menu |
| HAP (HomeKit) enabled | off | on/off, requires Wi-Fi (turns it on); change → confirm → reboot | menu |
| LCD backlight | on | on/off | menu, HomeKit custom "Display" |
| LCD backlight sleep | never | never / 10 s / 5 min / 1 h / 6 h / 12 h / 1 d / 3 d / 1 w / 1 mo | menu |
| Energy price | 0.27 EUR/kWh | 0–9.99, 0.01 step; used for the savings figure on the LCD | menu |
| Telemetry counter auto-reset | never | never / day / week / month / year | menu |
| Serial telemetry mode | 1 (all) | 0 off, 1 all, 2 essential, 3 numbers | menu |
| Wh, kWh, run time | 0 | persisted counters, see 3.4 | automatic |

### 3.2 Battery presets

The preset sets max/min voltage; the user can still edit both afterwards.

| Preset | Max (absorption) | Min (empty) | Notes |
|---|---|---|---|
| None (PSU) | user-set output voltage | — | Battery service reports 100 %, "not chargeable" |
| 12 V lead-acid | 14.40 V | 11.80 V | |
| 24 V lead-acid | 28.80 V | 23.60 V | FUGU default 27.3 / 22.4 kept as an alternative "24 V AGM float" preset |
| 12 V LiFePO4 (4S) | 14.40 V | 12.00 V | |
| 24 V LiFePO4 (8S) | 28.80 V | 24.00 V | |
| Custom | user-set | user-set | |

Default preset is **None**. Hardware limit `vOutSystemMax = 50 V` (as in FUGU), so 48 V
systems are not offered. Preset voltages are proposals, to be confirmed before phase 2.

### 3.3 Compile-time parameters (Kconfig, `menuconfig → MPPT`)

- All GPIO numbers from 1.1.
- I2C: bus 0/1 pins, frequency (400 kHz), ADS1115 address (0x48), ADS1115 channel assignment
  (Vin = 3, Vout = 0, current = 2, temperature = 1), PGA (±4.096 V), data rate (860 SPS),
  use of RDY pin.
- LCD: PCF8574 address (0x27), pin mapping (RS = P0, RW = P1, E = P2, BL = P3, D4–D7 = P4–P7).
- Calibration: `inVoltageDivRatio` 40.2156, `outVoltageDivRatio` 24.5, current scale 1.33,
  `currentSensV` 0.066 V/A, `currentMidPoint` 2.525 V, averaging counts (3 / 4 / 500),
  `voltageDropout` 1.0 V, `voltageBatteryThresh` 1.5 V, `currentInAbsolute` 31 A,
  `currentOutAbsolute` 50 A, `vInSystemMin` 10 V, `vOutSystemMax` 50 V, `PPWM_margin` 99.5 %,
  `PWM_MaxDC` 97 %, `efficiencyRate` 1.0.
- PWM: frequency 39 kHz, resolution 11 bit (LEDC low-speed mode), P&O step 1.
- NTC: R25, B, R_fixed, Vdd, circuit mode, TH2 ADC channel.
- Timing: routine interval 250 ms, LCD interval 1 s, HAP push interval 2 s, error window 1 s,
  error count limit 5, menu timeout 7 s, Wh persist interval.
- HomeKit: manufacturer, model, hardware revision, setup code (test builds only), name pattern.
- Console: UART0 (default, also used for flashing) or USB CDC on the mini-USB port.

### 3.4 Persistence of energy counters

Wh / run-time counters are written to NVS:

- once per hour while the charger is active;
- immediately on the transition to input under-voltage (IUV, sunset) or when the buck is
  disabled by a fault, since these precede a possible power loss;
- on every settings change (settings and counters are separate NVS keys).

The MCU stays powered from the battery side at night (FUGU `inputSource = 2`), so the sunset
write is not a race against power loss.

---

## 4. LCD and buttons

Three buttons replace FUGU's four (Left / Right / Back / Select). Navigation is a standard
numbered list menu.

| Context | UP | DOWN | MENU short | MENU long (2 s) |
|---|---|---|---|---|
| Display pages | previous page | next page | open menu | — (10 s: factory reset, see below) |
| Menu list | scroll up | scroll down | select item | exit (same as "Exit") |
| Value editing | value + (hold = auto-repeat) | value − (hold = auto-repeat) | confirm and save | cancel edit |

The two LCD lines show two consecutive menu items with a marker on the active one; DOWN scrolls
to items 3–4, UP back to 1–2.

Rules:

- Every list item has a number; **Exit is always the last item**. In a sub-menu, Exit returns
  to the previous level; at the top level it returns to the display pages.
- 7 s without a key press at any level returns to the display pages; an unconfirmed edit is
  discarded.
- MENU long press is free in this scheme (FUGU used long Select only to enter settings), so it
  is mapped to Exit/Cancel at 2 s.
- MENU held for more than 10 s triggers Factory Reset (after 2 s the display shows "hold for
  factory reset" with a countdown; releasing earlier only exits).

Display pages (from FUGU): 1 power + energy + SOC + Vout + Iout; 2 input and output V/A;
3 energy + SOC bar graph; 4 temperature + fan; 5 energy savings (kWh × price).

Menu items: FUGU's settings (charging mode, output mode, battery max/min, charging current,
fan, fan temperature, shutdown temperature, backlight sleep, counter reset, save/autoload) plus
battery preset and energy price, and a **Device Setup** sub-menu:

| # | Item | Behaviour |
|---|---|---|
| 1 | Enable HAP / Disable HAP | label reflects the current state; turning on also turns Wi-Fi on; confirm → reboot |
| 2 | Enable WiFi / Disable WiFi | shown only while HAP is off |
| 3 | FW Update | checks `otafw` for a newer version and installs it; progress on the LCD |
| 4 | Reset WiFi | erases credentials, restarts the captive portal |
| 5 | Factory Reset | confirm → erase settings, counters, Wi-Fi and HAP pairing → reboot |
| 6 | Info | firmware version, IP, RSSI, HAP pairing state |
| 7 | Exit | back to the main menu |

---

## 5. HomeKit model

One accessory (category Switch), enabled only when the "HAP enabled" setting is on.

| Service | Characteristics | Source |
|---|---|---|
| Accessory Information | name `MPPT-xxxxxx`, manufacturer, model, serial = Wi-Fi MAC, firmware = `CONFIG_APP_PROJECT_VER`, hardware = 1.1 | Kconfig |
| Switch "Charger" | On = charging enabled | `chargingPause` inverted |
| ↳ custom (existing) | Display (LCD backlight), Firmware Update trigger, FW Update Status | existing template |
| ↳ custom (writable) | Output Mode, MPPT Mode, Battery Max V, Battery Min V, Charging Current | menu settings |
| ↳ custom (read-only, later) | Vin, Iin, Vout, Iout, PWM, error bitmask, Wh (Eve UUIDs for Eve app history) | deferred |
| Battery Service | Battery Level = SOC %, Charging State = charging / not charging / not chargeable (PSU or preset "None"), Status Low Battery (< 10 %) | Blynk LED1–3 |
| Temperature Sensor | Current Temperature = TH1, Status Fault = OTE | display page 4 |
| Fan v1 | On = fan running; write = fan override | System_Processes |

With battery preset "None" the Battery service reports **100 %** and "not chargeable", so the
Home app does not raise low-battery warnings.

HAP pairing reset: IO0 held 3 s resets Wi-Fi credentials, 10 s resets to factory; both also
available from the Device Setup menu, and factory reset also via MENU held 10 s.

---

## 6. Partition table and OTA

Project code name (CMake project and binary name): **`mppt1hs2`**.

`partitions_hap.csv`: 4 MB flash, `sec_cert`, `nvs`, `otadata`, `phy_init`, `ota_0` / `ota_1`
(1600 KB each), `factory_nvs`, `nvs_keys`.

OTA through `fupdateota` (ESP32 path: `esp_https_ota_begin` → image descriptor → version
compare → download → `esp_https_ota_finish`; the application reboots on
`FW_UPG_STATUS_SUCCESS`), URL
`https://raw.githubusercontent.com/AramVartanyan/otafw/master/mppt1hs2.bin`, certificate bundle
enabled. The firmware version is `CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults`
(`CONFIG_APP_PROJECT_VER_FROM_CONFIG=y`), which ESP-IDF writes into the app descriptor; that is
what HomeKit reports as Firmware Revision and what the OTA version check compares
(`major.minor.patch`, only a strictly newer image is installed). An automatic check runs
1 minute after Wi-Fi connects; a manual check/update is in Device Setup → FW Update.

Required changes in `fupdateota` (separate repository, own PR):

1. `otaUpdate()` refuses to run while `hap_get_paired_controller_count() == 0`. With HAP
   disabled this blocks OTA entirely, so the pairing guard becomes optional (Kconfig, default
   on for HomeKit-only devices, off here) or moves to the caller.
2. A check-only call (`otaCheckVersion()`: fetch the descriptor, compare, abort without
   writing) so the menu can show "up to date" / "vX.Y.Z available" and the automatic check
   does not download an image that is then rejected.

---

## 7. Implementation phases

Each phase is reviewed and approved before the next starts.

1. **Infrastructure** — `esp32s2` target, `sdkconfig.defaults`, Kconfig with MPPT32 pins,
   CMake and `idf_component.yml`, ported `ads1115`, module skeletons. Builds; buck disabled.
2. **Measurement and UI** — `mppt_hal`, `mppt_sensors`, LCD pages, menu, NVS settings, log
   telemetry. Tested from USB power without the power stage.
3. **Control** — `mppt_control`: protection and charging algorithm. First tests with a
   laboratory PSU instead of a panel, then PV.
4. **Connectivity** — `captive-wifi` component, Device Setup menu, OTA check; then
   `mppt_hap` and replacement of the template callbacks.
5. **Extras** — web status page on the device IP, TH2 redundancy, Wh persistence tuning, Eve
   characteristics.

---

## 8. Open items

- L8 parameters (see 1.5).
- NTC B constant after calibration.
- Battery preset voltages (3.2) to be confirmed.
- HomeKit Fan service and custom characteristics: final configuration after testing with
  Apple's tools.
- Console: UART0 for development; USB CDC to be evaluated later.

---

## 9. Licence and credits

Firmware: MIT (see `LICENSE`). Charging algorithm, protection logic and calibration constants
derived from FUGU-ARDUINO-MPPT-FIRMWARE by Angelo Casimiro (TechBuilder), released under
CC0 1.0. ADS1115 component by Blake Felt (Molorius). HD44780 driver by esp-idf-lib.
