#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "outputwrite.h"
#include "ads1115.h"
#include "hd44780.h"
#include "ntc_driver.h"
#include "mppt_config.h"
#include "mppt_hal.h"

static const char *TAG = "mppt_hal";

static i2c_master_bus_handle_t s_bus0;      /* ADS1115 */
static i2c_master_bus_handle_t s_bus1;      /* LCD */
static i2c_master_dev_handle_t s_lcd_dev;   /* PCF8574 */
static ads1115_t s_ads;
static bool s_ads_ok;
static hd44780_t s_lcd;
static bool s_lcd_ok;
static ntc_device_handle_t s_ntc2;
static uint32_t s_pwm_max;

/* ------------------------------------------------------------------ safe */

esp_err_t mppt_hal_safe_outputs(void)
{
    esp_err_t err = ESP_OK;
    err |= ioInit(MPPT_GPIO_BUCK_EN, true, false);
    OutputWrite(false, MPPT_GPIO_BUCK_EN, false);
    err |= ioInit(MPPT_GPIO_BACKFLOW, true, false);
    OutputWrite(false, MPPT_GPIO_BACKFLOW, false);
    err |= ioInit(MPPT_GPIO_FAN, true, false);
    OutputWrite(false, MPPT_GPIO_FAN, false);
    /* PWM pin low until LEDC takes it over */
    err |= ioInit(MPPT_GPIO_PWM, true, false);
    OutputWrite(false, MPPT_GPIO_PWM, false);
    err |= ioInit(MPPT_GPIO_PWR12_GOOD, false, false);
    return err ? ESP_FAIL : ESP_OK;
}

/* ------------------------------------------------------------------- PWM */

static esp_err_t pwm_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,     /* the only mode on ESP32-S2 */
        .duty_resolution = (ledc_timer_bit_t)MPPT_PWM_RESOLUTION_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = MPPT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        return err;
    }
    ledc_channel_config_t ccfg = {
        .gpio_num = MPPT_GPIO_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        return err;
    }
    s_pwm_max = (1u << MPPT_PWM_RESOLUTION_BITS) - 1;
    return ESP_OK;
}

uint32_t mppt_hal_pwm_max(void)
{
    return s_pwm_max;
}

void mppt_hal_set_pwm(uint32_t duty)
{
    if (duty > s_pwm_max) {
        duty = s_pwm_max;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void mppt_hal_buck_enable(bool on)
{
    OutputWrite(on, MPPT_GPIO_BUCK_EN, false);
}

void mppt_hal_backflow(bool on)
{
    OutputWrite(on, MPPT_GPIO_BACKFLOW, false);
}

void mppt_hal_fan(bool on)
{
    OutputWrite(on, MPPT_GPIO_FAN, false);
}

bool mppt_hal_pwr12_good(void)
{
    return ReadInput(MPPT_GPIO_PWR12_GOOD) != 0;
}

/* ------------------------------------------------------------------- I2C */

static esp_err_t bus_init(i2c_port_num_t port, int sda, int scl, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  /* 10 kΩ pull-ups on the board */
    };
    return i2c_new_master_bus(&cfg, out);
}

/* --------------------------------------------------------------- ADS1115 */

static esp_err_t ads_init(void)
{
    s_ads = ads1115_config(s_bus0, MPPT_ADS_ADDR);
    if (s_ads.dev == NULL) {
        return ESP_FAIL;
    }
    ads1115_set_pga(&s_ads, (ads1115_fsr_t)MPPT_ADS_PGA_INDEX);
    ads1115_set_sps(&s_ads, ADS1115_SPS_860);
    ads1115_set_mode(&s_ads, ADS1115_MODE_SINGLE);
#ifdef CONFIG_MPPT_ADS_USE_RDY_PIN
    ads1115_set_rdy_pin(&s_ads, MPPT_GPIO_ADS_RDY);
#endif
    s_ads_ok = true;
    return ESP_OK;
}

esp_err_t mppt_hal_ads_read(uint8_t channel, float *volts)
{
    if (!s_ads_ok || channel > 3) {
        return ESP_ERR_INVALID_STATE;
    }
    ads1115_set_mux(&s_ads, (ads1115_mux_t)(ADS1115_MUX_0_GND + channel));
    *volts = (float)ads1115_get_voltage(&s_ads);
    return ESP_OK;
}

/* ------------------------------------------------------------------- LCD */

#ifdef CONFIG_MPPT_LCD_ENABLE
/* PCF8574 backpack: one byte per write, bit positions from Kconfig. The
 * hd44780 driver hands us a byte laid out per hd44780_t.pins. */
static esp_err_t lcd_write_cb(const hd44780_t *lcd, uint8_t data)
{
    (void)lcd;
    return i2c_master_transmit(s_lcd_dev, &data, 1, 20);
}

static esp_err_t lcd_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_MPPT_LCD_ADDR,
        .scl_speed_hz = 100000,                /* PCF8574 is a 100 kHz part */
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus1, &dev_cfg, &s_lcd_dev);
    if (err != ESP_OK) {
        return err;
    }
    s_lcd = (hd44780_t){
        .write_cb = lcd_write_cb,
        .font = HD44780_FONT_5X8,
        .lines = 2,
        .pins = {
            .rs = CONFIG_MPPT_LCD_PIN_RS,
            .e  = CONFIG_MPPT_LCD_PIN_E,
            .d4 = CONFIG_MPPT_LCD_PIN_D4,
            .d5 = CONFIG_MPPT_LCD_PIN_D4 + 1,
            .d6 = CONFIG_MPPT_LCD_PIN_D4 + 2,
            .d7 = CONFIG_MPPT_LCD_PIN_D4 + 3,
            .bl = CONFIG_MPPT_LCD_PIN_BL,
        },
        .backlight = true,
    };
    err = hd44780_init(&s_lcd);
    if (err != ESP_OK) {
        return err;
    }
    s_lcd_ok = true;
    return ESP_OK;
}
#endif

bool mppt_hal_lcd_present(void)
{
    return s_lcd_ok;
}

const hd44780_t *mppt_hal_lcd(void)
{
    return s_lcd_ok ? &s_lcd : NULL;
}

esp_err_t mppt_hal_lcd_backlight(bool on)
{
    if (!s_lcd_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    return hd44780_switch_backlight(&s_lcd, on);
}

/* ------------------------------------------------------------------- NTC2 */

static esp_err_t ntc2_init(void)
{
#ifdef CONFIG_MPPT_NTC2_ENABLE
    ntc_config_t cfg = {
        .b_value = MPPT_NTC_BETA,
        .r25_ohm = MPPT_NTC_R25_OHM,
        .fixed_ohm = MPPT_NTC_RFIXED_OHM,
        .vdd_mv = MPPT_NTC_VDD_MV,
        .circuit_mode = MPPT_NTC_TO_VCC ? CIRCUIT_MODE_NTC_VCC : CIRCUIT_MODE_NTC_GND,
        .atten = ADC_ATTEN_DB_12,
        .channel = (adc_channel_t)CONFIG_MPPT_NTC2_ADC_CHANNEL,
        .unit = (CONFIG_MPPT_NTC2_ADC_UNIT == 1) ? ADC_UNIT_1 : ADC_UNIT_2,
    };
    adc_oneshot_unit_handle_t adc = NULL;
    return ntc_dev_create(&cfg, &s_ntc2, &adc);
#else
    return ESP_OK;
#endif
}

esp_err_t mppt_hal_ntc2_read(float *temp_c)
{
    if (!s_ntc2) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO(phase 2): return ESP_ERR_INVALID_STATE while Wi-Fi is active (ADC2). */
    return ntc_dev_get_temperature(s_ntc2, temp_c);
}

/* ------------------------------------------------------------------ init */

esp_err_t mppt_hal_init(void)
{
    esp_err_t err;

    err = pwm_init();
    if (err != ESP_OK) {
        return err;
    }
    mppt_hal_set_pwm(0);

    err = bus_init(I2C_NUM_0, MPPT_I2C0_SDA, MPPT_I2C0_SCL, &s_bus0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus 0: %s", esp_err_to_name(err));
        return err;
    }
    err = bus_init(I2C_NUM_1, MPPT_I2C1_SDA, MPPT_I2C1_SCL, &s_bus1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus 1: %s", esp_err_to_name(err));
        return err;
    }

    if (ads_init() != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not found at 0x%02x", MPPT_ADS_ADDR);
    } else {
        ESP_LOGI(TAG, "ADS1115 ready at 0x%02x", MPPT_ADS_ADDR);
    }

#ifdef CONFIG_MPPT_LCD_ENABLE
    if (lcd_init() != ESP_OK) {
        ESP_LOGW(TAG, "LCD not found at 0x%02x", CONFIG_MPPT_LCD_ADDR);
    } else {
        ESP_LOGI(TAG, "LCD ready at 0x%02x", CONFIG_MPPT_LCD_ADDR);
    }
#endif

    if (ntc2_init() != ESP_OK) {
        ESP_LOGW(TAG, "TH2 (on-chip ADC) not available");
    }

    return ESP_OK;
}
