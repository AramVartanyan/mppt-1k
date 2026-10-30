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
- **TH2** is on IO14 (ADC2), which is unreliable while Wi-Fi is on. Move it to
  an ADC1 pin (GPIO1–10).
- **R27** (0 Ω, TH1 to IO1) as a backup path for the temperature reading:
  confirm whether it is fitted and whether the firmware should use it.

## Calibration

- Measure the NTC (TH1) R25 and B and enter them (Beta formula; defaults for
  now).
- L8 parameters.
- ACS712 midpoint auto-calibration: guard against calibrating while current
  flows.

## Firmware

- Phase 3: protections (on a fault, `SDWN` first, then PWM = 0), backflow,
  P&O MPPT / CC-CV, PSU mode, fan, PWR12 check. Several consecutive ADS1115
  read failures count as a fault.
- Phase 4: captive portal and HomeKit accessory; test the fan HomeKit service.
- OTA, before the unit goes into its enclosure.
- USB CDC console.
- `button` library: `gpio_install_isr_service` without a check logs
  "GPIO isr service already installed" (check whether the fixed version is in
  use).
- LCD and menu: test once the display wiring is fixed.
