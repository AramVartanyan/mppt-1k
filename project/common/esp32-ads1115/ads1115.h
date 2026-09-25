/*
 * ADS1115 16-bit I2C ADC driver for ESP-IDF.
 *
 * Based on esp32-ads1115 by Blake Felt (Molorius), ported to the ESP-IDF 5.x
 * driver/i2c_master API. The public API is unchanged except for
 * ads1115_config(), which now takes an i2c_master bus handle instead of a
 * legacy i2c_port_t.
 */

#ifndef ADS1115_H
#define ADS1115_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { // register address
  ADS1115_CONVERSION_REGISTER_ADDR = 0,
  ADS1115_CONFIG_REGISTER_ADDR,
  ADS1115_LO_THRESH_REGISTER_ADDR,
  ADS1115_HI_THRESH_REGISTER_ADDR,
  ADS1115_MAX_REGISTER_ADDR
} ads1115_register_addresses_t;

typedef enum { // multiplex options
  ADS1115_MUX_0_1 = 0,
  ADS1115_MUX_0_3,
  ADS1115_MUX_1_3,
  ADS1115_MUX_2_3,
  ADS1115_MUX_0_GND,
  ADS1115_MUX_1_GND,
  ADS1115_MUX_2_GND,
  ADS1115_MUX_3_GND,
} ads1115_mux_t;

typedef enum { // full-scale resolution options
  ADS1115_FSR_6_144 = 0,
  ADS1115_FSR_4_096,
  ADS1115_FSR_2_048,
  ADS1115_FSR_1_024,
  ADS1115_FSR_0_512,
  ADS1115_FSR_0_256,
} ads1115_fsr_t;

typedef enum { // samples per second
  ADS1115_SPS_8 = 0,
  ADS1115_SPS_16,
  ADS1115_SPS_32,
  ADS1115_SPS_64,
  ADS1115_SPS_128,
  ADS1115_SPS_250,
  ADS1115_SPS_475,
  ADS1115_SPS_860
} ads1115_sps_t;

typedef enum {
  ADS1115_MODE_CONTINUOUS = 0,
  ADS1115_MODE_SINGLE
} ads1115_mode_t;

typedef union { // configuration register
  struct {
    uint16_t COMP_QUE:2;  // bits 0..  1  Comparator queue and disable
    uint16_t COMP_LAT:1;  // bit  2       Latching Comparator
    uint16_t COMP_POL:1;  // bit  3       Comparator Polarity
    uint16_t COMP_MODE:1; // bit  4       Comparator Mode
    uint16_t DR:3;        // bits 5..  7  Data rate
    uint16_t MODE:1;      // bit  8       Device operating mode
    uint16_t PGA:3;       // bits 9..  11 Programmable gain amplifier configuration
    uint16_t MUX:3;       // bits 12.. 14 Input multiplexer configuration
    uint16_t OS:1;        // bit  15      Operational status or single-shot conversion start
  } bit;
  uint16_t reg;
} ADS1115_CONFIG_REGISTER_Type;

typedef struct {
  bool in_use;                 // gpio is used
  gpio_num_t pin;              // ready pin
  QueueHandle_t gpio_evt_queue; // pin triggered queue
} ads1115_rdy_pin_t;

typedef struct {
  ADS1115_CONFIG_REGISTER_Type config;
  i2c_master_dev_handle_t dev;   // device handle on the i2c_master bus
  int address;
  ads1115_rdy_pin_t rdy_pin;
  ads1115_register_addresses_t last_reg; // save last accessed register
  bool changed;                // save if a value was changed or not
  int timeout_ms;              // maximum wait for an i2c transaction
} ads1115_t;

/* Initialize device: adds the ADS1115 at `address` to an already created
 * i2c_master bus and returns the default configuration (single-shot, ±4.096 V,
 * 64 SPS, AIN0-GND). On failure `dev` is NULL and every read returns 0. */
ads1115_t ads1115_config(i2c_master_bus_handle_t bus, uint8_t address);

/* Remove the device from the bus (does not delete the bus). */
void ads1115_delete(ads1115_t* ads);

// set configuration
void ads1115_set_rdy_pin(ads1115_t* ads, gpio_num_t gpio); // set up data-ready pin
void ads1115_set_mux(ads1115_t* ads, ads1115_mux_t mux); // set multiplexer
void ads1115_set_pga(ads1115_t* ads, ads1115_fsr_t fsr); // set fsr
void ads1115_set_mode(ads1115_t* ads, ads1115_mode_t mode); // set read mode
void ads1115_set_sps(ads1115_t* ads, ads1115_sps_t sps); // set sampling speed
void ads1115_set_max_ticks(ads1115_t* ads, TickType_t max_ticks); // maximum wait ticks for i2c bus

int16_t ads1115_get_raw(ads1115_t* ads); // get voltage in bits
double ads1115_get_voltage(ads1115_t* ads); // get voltage in volts

#ifdef __cplusplus
}
#endif

#endif // ifdef ADS1115_H
