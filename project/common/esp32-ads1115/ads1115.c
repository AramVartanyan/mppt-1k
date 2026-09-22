/*
 * ADS1115 driver, ESP-IDF 5.x i2c_master port of esp32-ads1115 (Blake Felt).
 *
 * Changes against the original:
 *  - driver/i2c_master transactions instead of the legacy i2c_cmd_link API
 *  - QueueHandle_t (xQueueHandle was removed in IDF 5)
 *  - RDY pin: 64-bit pin mask (GPIO >= 32), GPIO_INTR_NEGEDGE, ISR handler
 *    installed once, bounded wait instead of portMAX_DELAY
 */

#include "ads1115.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "ads1115";

static void IRAM_ATTR gpio_isr_handler(void* arg) {
  const bool ret = 1; // dummy value to pass to queue
  QueueHandle_t gpio_evt_queue = (QueueHandle_t) arg; // find which queue to write
  xQueueSendFromISR(gpio_evt_queue, &ret, NULL);
}

static esp_err_t ads1115_write_register(ads1115_t* ads, ads1115_register_addresses_t reg, uint16_t data) {
  if (ads->dev == NULL) return ESP_ERR_INVALID_STATE;
  uint8_t out[3];
  out[0] = (uint8_t)reg;      // register pointer
  out[1] = data >> 8;         // 8 greater bits
  out[2] = data & 0xFF;       // 8 lower bits
  esp_err_t ret = i2c_master_transmit(ads->dev, out, sizeof(out), ads->timeout_ms);
  ads->last_reg = reg; // change the internally saved register
  return ret;
}

static esp_err_t ads1115_read_register(ads1115_t* ads, ads1115_register_addresses_t reg, uint8_t* data, uint8_t len) {
  if (ads->dev == NULL) return ESP_ERR_INVALID_STATE;
  esp_err_t ret;
  if (ads->last_reg != reg) { // if we're not on the correct register, change it
    uint8_t r = (uint8_t)reg;
    ret = i2c_master_transmit_receive(ads->dev, &r, 1, data, len, ads->timeout_ms);
    if (ret == ESP_OK) ads->last_reg = reg;
    return ret;
  }
  return i2c_master_receive(ads->dev, data, len, ads->timeout_ms);
}

ads1115_t ads1115_config(i2c_master_bus_handle_t bus, uint8_t address) {
  ads1115_t ads = {0}; // setup configuration with default values
  ads.config.bit.OS = 1; // always start conversion
  ads.config.bit.MUX = ADS1115_MUX_0_GND;
  ads.config.bit.PGA = ADS1115_FSR_4_096;
  ads.config.bit.MODE = ADS1115_MODE_SINGLE;
  ads.config.bit.DR = ADS1115_SPS_64;
  ads.config.bit.COMP_MODE = 0;
  ads.config.bit.COMP_POL = 0;
  ads.config.bit.COMP_LAT = 0;
  ads.config.bit.COMP_QUE = 0b11;

  ads.address = address; // save i2c address
  ads.rdy_pin.in_use = 0; // state that rdy_pin not used
  ads.last_reg = ADS1115_MAX_REGISTER_ADDR; // say that we accessed invalid register last
  ads.changed = 1; // say we changed the configuration
  ads.timeout_ms = 10;

  i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = address,
    .scl_speed_hz = 400000,
  };
  esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &ads.dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "could not add device 0x%02x: %s", address, esp_err_to_name(err));
    ads.dev = NULL;
  }
  return ads; // return the completed configuration
}

void ads1115_delete(ads1115_t* ads) {
  if (ads->rdy_pin.in_use) {
    gpio_isr_handler_remove(ads->rdy_pin.pin);
    vQueueDelete(ads->rdy_pin.gpio_evt_queue);
    ads->rdy_pin.in_use = 0;
  }
  if (ads->dev) {
    i2c_master_bus_rm_device(ads->dev);
    ads->dev = NULL;
  }
}

void ads1115_set_mux(ads1115_t* ads, ads1115_mux_t mux) {
  ads->config.bit.MUX = mux;
  ads->changed = 1;
}

void ads1115_set_rdy_pin(ads1115_t* ads, gpio_num_t gpio) {
  gpio_config_t io_conf = {
    .intr_type = GPIO_INTR_NEGEDGE, // positive to negative (pulled down)
    .pin_bit_mask = 1ULL << gpio,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
  };
  gpio_config(&io_conf); // set gpio configuration

  ads->rdy_pin.gpio_evt_queue = xQueueCreate(1, sizeof(bool));
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
  }
  gpio_isr_handler_add(gpio, gpio_isr_handler, (void*)ads->rdy_pin.gpio_evt_queue);

  ads->rdy_pin.in_use = 1;
  ads->rdy_pin.pin = gpio;
  ads->config.bit.COMP_QUE = 0b00; // assert after one conversion
  ads->changed = 1;

  err = ads1115_write_register(ads, ADS1115_LO_THRESH_REGISTER_ADDR, 0); // set lo threshold to minimum
  if (err) ESP_LOGE(TAG, "could not set low threshold: %s", esp_err_to_name(err));
  err = ads1115_write_register(ads, ADS1115_HI_THRESH_REGISTER_ADDR, 0xFFFF); // set hi threshold to maximum
  if (err) ESP_LOGE(TAG, "could not set high threshold: %s", esp_err_to_name(err));
}

void ads1115_set_pga(ads1115_t* ads, ads1115_fsr_t fsr) {
  ads->config.bit.PGA = fsr;
  ads->changed = 1;
}

void ads1115_set_mode(ads1115_t* ads, ads1115_mode_t mode) {
  ads->config.bit.MODE = mode;
  ads->changed = 1;
}

void ads1115_set_sps(ads1115_t* ads, ads1115_sps_t sps) {
  ads->config.bit.DR = sps;
  ads->changed = 1;
}

void ads1115_set_max_ticks(ads1115_t* ads, TickType_t max_ticks) {
  ads->timeout_ms = (int)(max_ticks * portTICK_PERIOD_MS);
  if (ads->timeout_ms < 1) ads->timeout_ms = 1;
}

int16_t ads1115_get_raw(ads1115_t* ads) {
  const static uint16_t sps[] = {8, 16, 32, 64, 128, 250, 475, 860};
  const static uint8_t len = 2;
  uint8_t data[2];
  esp_err_t err;
  bool tmp; // temporary bool for reading from queue

  // conversion time in ms, rounded up, plus 1 ms margin
  const uint32_t conv_ms = (1000 + sps[ads->config.bit.DR] - 1) / sps[ads->config.bit.DR] + 1;

  if (ads->rdy_pin.in_use) {
    xQueueReset(ads->rdy_pin.gpio_evt_queue);
  }
  // see if we need to send configuration data
  if ((ads->config.bit.MODE == ADS1115_MODE_SINGLE) || (ads->changed)) { // if it's single-ended or a setting changed
    err = ads1115_write_register(ads, ADS1115_CONFIG_REGISTER_ADDR, ads->config.reg);
    if (err) {
      ESP_LOGE(TAG, "could not write to device: %s", esp_err_to_name(err));
      return 0;
    }
    ads->changed = 0; // say that the data is unchanged now
  }

  if (ads->rdy_pin.in_use) {
    // bounded wait: twice the conversion time plus margin, never forever
    if (xQueueReceive(ads->rdy_pin.gpio_evt_queue, &tmp, pdMS_TO_TICKS(2 * conv_ms + 10)) != pdTRUE) {
      ESP_LOGW(TAG, "RDY timeout, reading anyway");
    }
  } else {
    vTaskDelay(pdMS_TO_TICKS(conv_ms) + 1);
  }

  err = ads1115_read_register(ads, ADS1115_CONVERSION_REGISTER_ADDR, data, len);
  if (err) {
    ESP_LOGE(TAG, "could not read from device: %s", esp_err_to_name(err));
    return 0;
  }
  return (int16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

double ads1115_get_voltage(ads1115_t* ads) {
  const double fsr[] = {6.144, 4.096, 2.048, 1.024, 0.512, 0.256};
  const int16_t bits = (1L << 15) - 1;
  int16_t raw;

  raw = ads1115_get_raw(ads);
  return (double)raw * fsr[ads->config.bit.PGA] / (double)bits;
}
