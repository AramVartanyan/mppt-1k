# TODO

Open items for the MPPT controller (`project/mppt-hap`). Items are removed
once done.

## Hardware (next board revision)

- **Short-circuit protection.** There is no hardware overcurrent cut-off: the
  IR2104 gate driver has only UVLO and the ACS712 has no fault output, so the
  converter relies on the software loop (~20 ms) and the fuses. With 60 µH and
  50 V across the inductor the current rises ~0.8 A/µs, so only a hardware
  path is fast enough. Options to discuss after the firmware is finished:
  - a fast comparator (e.g. TLV3201, or LM393) on the ACS712 output against a
    threshold divider, open-drain output pulling IR2104 `SD#` low, also routed
    to a spare GPIO so the firmware can latch the fault;
  - or a current sensor with a built-in fault output (ACS710 / ACS711) wired
    to `SD#` the same way.
- **Wi-Fi LED on the TH2 footprint.** TH2 is not fitted; an LED sits in its
  place (anode to 3V3, cathode to IO14 and R39). With IO14 low nothing limits
  the current, so fit a series resistor (330 Ω – 1 kΩ), then set
  `CONFIG_LED_GPIO` = 14 and `CONFIG_LED_ACTIVE_LOW` = y. Until then IO14 stays
  an input (the LED glows faintly through R39). On the next board revision give
  the LED its own pin and resistor.
- **R27** (0 Ω, TH1 node to ADS1115 AIN1) must be fitted, otherwise AIN1
  floats and TH1 reads about −5 °C. TH1 is also wired directly to IO1
  (ADC1_CH0), which could serve as a backup reading.

## Calibration

- L8 parameters.
- ACS712 midpoint auto-calibration: guard against calibrating while current
  flows.

## Firmware

- Phase 3: protections (on a fault, `SDWN` first, then PWM = 0), backflow,
  P&O MPPT / CC-CV, PSU mode, fan, PWR12 check. Several consecutive ADS1115
  read failures count as a fault.
- Phase 4: captive portal and HomeKit accessory; test the fan HomeKit service.
- OTA, before the unit goes into its enclosure.
- LCD and menu: test once the display wiring is fixed.
