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
- **Status LED** has no pin of its own and sits on the TH2 footprint (IO14). On
  the next board revision give it a dedicated pin, so TH2 and the LED can both
  be fitted.
- **Output capacitors C13/C14** are rated 35 V, so the output is limited to
  35 V (`CONFIG_MPPT_CAL_VOUT_SYS_MAX_MV`). Replace them with 63 V parts, then
  set the limit back to FUGU's 50 V.
- **R27** (0 Ω, TH1 node to ADS1115 AIN1) must be fitted, otherwise AIN1
  floats and TH1 reads about −5 °C. TH1 is also wired directly to IO1
  (ADC1_CH0), which could serve as a backup reading.

## Calibration

- L8 parameters.
- ACS712 midpoint auto-calibration: guard against calibrating while current
  flows.

## Firmware

- Phase 3, in the steps of README section 7 (simple first, PSU from panels
  last). Also: output over-voltage trips at `vOutSystemMax` itself, not only
  at battery max + threshold; repeated faults latch until confirmed on the LCD
  (README 2.5, item 8).
- Phase 4: captive portal and HomeKit accessory; test the fan HomeKit service.
- OTA: next, before phase 3 (flashing over USB works, but updates in the
  enclosure should not need a cable).
