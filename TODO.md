# TODO

Open items for the MPPT controller (`project/mppt-hap`). Items are removed
once done.

## Hardware (boards already built)

- **Output capacitors C13/C14**: fit 63 V parts (35 V parts are on the first
  board). Then set `CONFIG_MPPT_CAL_VOUT_SYS_MAX_MV` back to FUGU's 50000.
- Assembly steps for every unit are in README 1.6.

## Hardware (next board revision)

- **Short-circuit protection in hardware.** MPPT32 v1.1 has none and will not
  get one: the IR2104 has only UVLO, the ACS712 has no fault output, and its
  FILTER pin carries C7 = 2.2 µF, which with the internal 1.7 kΩ limits the
  sensor output to about 40 Hz. The firmware loop (~20 ms) and the 30 A fuses
  are all there is. With L8 = 100 µH (less under DC bias) and up to 80 V
  across it the current rises at 0.8 A/µs or more, so the cut-off must be
  analog and act within a few µs. Recommendations:
  - **Comparator on a fast current signal, latched, into `SD#`.** Take the
    signal before the slow filter: either reduce C7 to about 1 nF (ACS712
    bandwidth back to ~80 kHz, response ~5 µs; the firmware already averages)
    or add a separate sense path. Use a fast comparator (e.g. TLV3201,
    ~40 ns) against a threshold derived from the same 5 V as the ACS712
    (ratiometric), e.g. 22 A ≈ 2.5 V + 22 × 66 mV = 3.95 V, with a little
    hysteresis. Its open-drain output sets a latch (e.g. 74LVC1G74 with
    preset/clear) whose output pulls IR2104 `SD#` low and goes to a GPIO
    interrupt; the firmware reads the fault, keeps `SDWN` low and clears the
    latch only after the cause is gone. Without the latch the driver would
    restart on the next PWM cycle.
  - **Faster alternative: low-side shunt.** A 1–2 mΩ shunt in the ground
    return of the low-side MOSFETs with the same comparator and latch reacts
    in well under 1 µs and also allows a cycle-by-cycle current limit.
  - **Simpler alternative: ACS710 / ACS711.** Hall sensors with a built-in
    over-current fault output (~1–2 µs) wired to the latch and `SD#` the same
    way; the threshold is set with resistors.
  - Keep a pull-down on `SD#` so the driver stays off while the MCU resets
    or boots, and keep the fuses as the last resort.
- **Status LED** has no pin of its own and sits on the TH2 footprint (IO14).
  Give it a dedicated pin with a series resistor, so TH2 and the LED can both
  be fitted.
- **LCD I2C level shifter.** The LCD backpack runs at 5 V (PCF8574 V_IH =
  0.7 × VDD = 3.5 V) while the ESP32-S2 is a 3.3 V part. Add a PCA9306 or two
  BSS138 MOSFETs with pull-ups on both sides, so the backpack's own pull-ups
  (R8/R9) no longer have to be removed.
- **BOM / schematic:** C13/C14 63 V; L8 is 100 µH (the schematic says 60 µH).

## Calibration

- L8 under load: measure the current ripple at 15–16 A (README 1.5) in phase 3,
  step 4.
- ACS712 midpoint auto-calibration: guard against calibrating while current
  flows. To be discussed in detail in phase 3 (what can be detected, what
  cannot, and when the calibration may run).

## Firmware

- Phase 3, in the steps of README section 7 (simple first, PSU from panels
  last). `mppt_control_emergency_stop()` currently sets PWM = 0 before
  `SDWN`; change it to `SDWN` → PWM = 0 → backflow off, and add the PSU soft
  stop (README 3.2, "Stopping the power stage"). Also: output over-voltage trips at `vOutSystemMax` itself, not only
  at battery max + threshold; repeated faults latch until confirmed on the LCD
  (README 2.5, item 8).
- Phase 4: test the HomeKit accessory on the hardware (pairing, Outlet,
  energy read in Eve, energy price, temperature, fan rules, battery). Watch the
  free heap with HAP, Wi-Fi and an OTA download together on the ESP32-S2.
- OTA: next, before phase 3 (flashing over USB works, but updates in the
  enclosure should not need a cable).
