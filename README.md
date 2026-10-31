# mppt-1k

Open-source 1 kW MPPT solar charge controller firmware for the **ESP32-S2**, written in C on
**ESP-IDF v5.5.4**, with optional Apple **HomeKit** (HAP) support.

The charging logic is a port of Angelo Casimiro's
[FUGU-ARDUINO-MPPT-FIRMWARE](https://github.com/AngeloCasi/FUGU-ARDUINO-MPPT-FIRMWARE)
(V1.10, CC0 1.0 / public domain) from Arduino/ESP32 to ESP-IDF/ESP32-S2, running on the
**MPPT32 v1.1** board (`files/Schematic_MPPT32_2026-09-22.pdf`).

The main goal is **PSU mode**: a stable regulated output from PV panels **without a battery**,
which off-the-shelf MPPT chargers do not offer. Battery charging (CC-CV) is kept from FUGU as
the second mode.

> **Status: phase 2 (measurement and UI) done and running on the hardware** (sensors, LCD,
> menu, buttons, USB console). This document is the agreed specification. Next: OTA, then
> phase 3, control (section 7).

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
| Display | 16×2 HD44780 LCD with PCF8574 I2C backpack, T4 connector (5 V) | remove the backpack's SDA/SCL pull-ups R8, R9 (see 1.6) |
| Buttons | UP, DOWN, MENU, T5 connector, active low, no external pull-ups | internal pull-ups used |
| Thermistors | TH1, TH2, 10 kΩ NTC, on PCB between the TO-220 MOSFETs | TH2 footprint carries the Wi-Fi LED by default (see 1.1) |
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
| SDA2 / SCL2 (temporary) | IO16 / IO17 | I2C bus 1 | **temporary, test board only.** The hardware is designed for IO9/IO10 and those stay the firmware defaults. On the first prototype IO9, IO10, IO11 and the strapping pin IO46 are shorted together. R40/R41 then pull IO46 high and the chip cannot enter download mode (`boot:0x4`); for flashing, tie T4 pin 3 (SDA2) to pin 1 (GND) or remove R40/R41 and check that IO46 reads 0 V at reset. On the test board the SDA2/SCL2 traces are cut between the module and R40/R41, and the resistor side is wired to IO16/IO17 (module pins 9/10; neighbours IO15 and IO18 unused, so a solder bridge is harmless), so T4 and the pull-ups stay in use; the pins are selected only in that board's menuconfig (`Bus 1 SDA/SCL`). Avoid IO34 (next to strapping pin IO45). Internal pull-ups are enabled on both buses |
| READ | IO39 | in | ADS1115 ALERT/RDY (10 kΩ pull-up) |
| ADC0 | IO1 | ADC1_CH0 | TH1 (also wired to ADS1115 AIN1 through R27 0 Ω) |
| ADC1 | IO14 | ADC2_CH3 | TH2 (ADC2 is not usable while Wi-Fi is active on ESP32-S2). Default: Wi-Fi status LED on the TH2 footprint (anode to 3V3, cathode through 500 Ω to IO14 and R39, active low). An NTC can still be fitted there instead (see 1.3) |
| UP / DWN / MENU | IO5 / IO6 / IO7 | in, pull-up | buttons |
| IO0 | IO0 | in | boot button S2A; HomeKit reset button (3 s Wi-Fi reset, 10 s factory reset) |
| TXD0 / RXD0 | | UART0 | T6 header; console only when selected instead of USB |
| IO19 / IO20 | | USB D- / D+ | native USB: console, telemetry, flashing |

The MPPT32 v1.1 board has no dedicated status LED pin (D3 is a hardware indicator on the
backflow gate supply). The status LED is fitted on the TH2 footprint instead: anode to 3V3,
cathode through a 500 Ω series resistor to IO14, with R39 (10 kΩ) to GND, so it is active
low. The firmware drives it with the `led_indicator` engine from `outputwrite` (Wi-Fi state
as steady base, HAP identify, OTA blink, reset patterns, over-temperature);
`CONFIG_LED_GPIO` defaults to 14 and `CONFIG_LED_ACTIVE_LOW` to y. GPIO configuration goes through `outputwrite` (`ioInit`, `OutputWrite`,
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
`espressif/ntc_driver` terms). The fitted NTC is a Semitec 103AT-11:
R25 = 10 kΩ ±1 %, B25/85 = 3435 K ±1 %; with R_fixed = 10 kΩ and Vdd = 3300 mV.
Against the Semitec resistance table the Beta formula with B = 3435 is within
0.6 °C from 0 to 100 °C (1.1 °C at 0 °C), so no Steinhart-Hart fit is needed.

- **TH1 is the primary sensor**, read through ADS1115 AIN1 (16-bit, independent of Wi-Fi).
  AIN1 is connected to the TH1 node only through R27 (0 Ω); without R27 the input floats.
- **TH2 is redundant**, read through IO14 (ADC2) with `ntc_driver`, only while Wi-Fi is off.
  Off by default (`CONFIG_MPPT_NTC2_ENABLE`): the TH2 footprint carries the status LED. To use
  TH2, fit the NTC in place of the LED, enable TH2 and move `CONFIG_LED_GPIO` to a free pin
  (e.g. IO2); the build stops with an error if both are left on IO14.

### 1.4 Power for bench testing

USB 5 V feeds the 3.3 V LDO, so the MCU, ADS1115 and LCD run from USB alone without PV input
and without the power stage populated. FUGU's `inputSource = 0` state covers this. Phases 1
and 2 (below) can therefore be tested on a board with only the MCU, ADS1115, dividers,
thermistors, LCD and buttons fitted.

### 1.5 Pending hardware data

- L8: 60 µH per schematic, 31 turns. Inductance, DC resistance, wire and core dimensions to
  be taken from the datasheet or measured. Core material is unknown; saturation will be
  checked indirectly from ripple current under load.

---

### 1.6 Assembly notes

- **LCD backpack: remove R8 and R9** (4.7 kΩ, SDA/SCL pull-ups to the backpack's 5 V).
  With them, SDA/SCL idle at about 4.4 V against R40/R41 (10 kΩ to 3.3 V): above the
  ESP32-S2's 3.3 V I/O limit, and current flows back into the 3.3 V rail. The Wi-Fi PHY
  calibration then fails (`phy: pll_cap_ext 10` repeated, then `abort()` in
  `lock_acquire_generic`). Without R8/R9 the bus is pulled to 3.3 V by R40/R41 and the
  ESP32-S2 internal pull-ups, and the LCD works normally. Leave R7 (pull-up of the
  PCF8574 INT output, not on the connector) and the address pull-ups in place.

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
  - `general-ota` (new, MIT) — HTTPS firmware update without HomeKit ties, replaces
    `fupdateota`. See section 6.
- Managed components (`main/idf_component.yml`):
  - `espressif/ntc_driver` — NTC on the on-chip ADC (TH2).
  - `esp-idf-lib/hd44780` — HD44780 driver used standalone with a project-supplied PCF8574
    write callback over `i2c_master` (no `i2cdev`/`pcf8574` dependencies). It needs the
    header-only `esp-idf-lib/esp_idf_lib_helpers` for its delay macros.
- External components expected in the build tree but **not** part of this repository
  (`components/*.txt` point to them): `esp-homekit-sdk` (`homekit`), `button` (`iot_button`),
  `outputwrite`, `app_hap_setup_payload`, `qrcode`, `mdns` (registry; required by HomeKit and
  reused by `captive-wifi`).

### 2.1.1 Build layout

`project/mppt-hap/CMakeLists.txt` expects `HOMEKIT_PATH` (default: repository root) to contain
`components/button` and `components/homekit` (esp-homekit-sdk). `project/common` is taken
relative to the project folder (`../common`) and holds `esp32-ads1115`, `container_nvs`,
`captive-wifi`, `general-ota`, `outputwrite`, `app_hap_setup_payload` and `qrcode`. `set(COMPONENTS main)` limits the build to `main` and
its transitive `REQUIRES`, so other components in those folders (e.g. `app_wifi`) are not
compiled. Project and binary name: `mppt1hs2`.

```sh
export HOMEKIT_PATH=/path/to/tree   # optional, defaults to <repo>
cd project/mppt-hap
idf.py set-target esp32s2
idf.py build flash monitor
```

Phase 2 image: 1000 KB of the 1600 KB OTA slot (HomeKit SDK included).

### 2.2 Source layout (`project/mppt-hap/main`)

| File | Responsibility | FUGU origin |
|---|---|---|
| `app_main.c` | boot, safe outputs first, NVS, tasks, Wi-Fi / HAP start if enabled, IO0 reset button, OTA events | `setup()` |
| `project/common/general-ota` | HTTPS OTA: check-only, update, events (section 6) | — |
| `mppt_config.h` | pins and constants from Kconfig | `#define`s |
| `mppt_state.h` | shared measurement/state struct + mutex | globals |
| `mppt_hal.c/.h` | I2C buses, ADS1115, LCD callback, LEDC PWM, GPIO, NTC ADC | Arduino calls |
| `mppt_sensors.c/.h` | Read_Sensors, current-sensor auto-zero, power, SOC, Wh | `2_Read_Sensors.ino` |
| `mppt_control.c/.h` | Device_Protection, backflowControl, Charging_Algorithm, System_Processes | tabs 3, 4, 5 |
| `mppt_settings.c/.h` | load/save/factory defaults in NVS | `5_System_Processes.ino` EEPROM part |
| `mppt_lcd.c/.h` | display pages and 3-button menu | `8_LCD_Menu.ino` |
| `mppt_telemetry.c/.h` | periodic log line (`ESP_LOGI`) | `6_Onboard_Telemetry.ino` |
| `mppt_hap.c/.h` | HomeKit services, started only when enabled | replaces `7_Wireless_Telemetry.ino` (Blynk) |
| `mppt_state.c/.h` | shared state implementation (mutex) | globals |
| `mppt_system.c/.h` | reboot, Wi-Fi reset, HAP reset, factory reset (shared by IO0 and the menu) | — |
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
8. Repeated faults latch. As in FUGU, a protection fault (OTE, IOC, OOC, OOV, FLV) stops the
   power stage and it restarts by itself once the cause clears. If the same kind of fault
   comes back too often (default: 3 faults within 5 minutes, both Kconfig parameters), the
   stage stays off until the user confirms on the LCD. Normal states (IUV at sunset, BNC,
   REC) never count towards the latch.

Kept with changes: `electricalPrice` / `energySavings` stay (default 0.27 EUR/kWh), but the
price becomes a menu setting and the savings are shown on the LCD only, not in HomeKit.

Dropped: Blynk telemetry, Bluetooth flag, dual-core task pinning, the Arduino `String`
firmware info strings (replaced by `CONFIG_APP_PROJECT_VER`), the Light Sensor service of the
HomeKit template.

New relative to FUGU: 12 V rail power-good input (PWR12) checked before enabling the gate
driver, HomeKit, Wi-Fi captive portal, single-firmware Wi-Fi / HAP on/off from the menu, OTA
update via `general-ota` (automatic check 1 min after Wi-Fi connects, and on demand from the
menu).

### 2.6 `captive-wifi` component

Own component in `project/common/captive-wifi`, MIT, written against ESP-IDF 5.5 APIs. It
follows the idea of tonyp7/esp32-wifi-manager and the structure of the official
`examples/protocols/http_server/captive_portal` example. API (`captive_wifi.h`):
`captive_wifi_init(hostname, callback, ctx)`, `captive_wifi_start()`, `captive_wifi_stop()`,
credential helpers (`has`, `clear`), status (`is_connected`, `portal_active`, `ap_ssid`,
`get_ip`, `rssi`) and an event callback (STA connected / disconnected, portal started /
stopped). The application keeps `TakeStatusConnected(bool)` as the single place that maps
connection state to the LED and shared state, as the HomeKit template did. Target size:
500–600 lines of C plus ~6 KB embedded HTML, no dependencies beyond `esp_wifi`,
`esp_netif`, `esp_http_server`, `lwip`, `nvs_flash` and `mdns`. STA part (credentials in NVS
namespace `cwifi`, connect, reconnect, events) and the portal are done; mDNS comes with
HomeKit in phase 4.

Behaviour:

1. Start (only when the Wi-Fi setting is on): if credentials exist in NVS → STA, connect with
   retries; on success → mDNS, HAP (if enabled), `TakeStatusConnected(true)`.
2. No credentials, or N failed attempts → open SoftAP `MPPT-xxxxxx` (private project, at most
   a few devices; a password can be set in Kconfig) with captive portal: DNS catch-all + DHCP option 114, so the sign-in page
   opens automatically on iOS, Android and Windows.
3. Portal page: scanned networks with signal strength, password field, connect button,
   status. HTTP endpoints `/`, `/scan`, `/connect`, `/status`.
4. The device tries the chosen network while the portal stays up (the page polls
   `/status`; the radio may change channel, so the phone can drop the AP for a moment).
   Credentials are saved only after the network gives an IP, so a typo never replaces a
   working network. 10 s later the portal stops and the device stays in STA, no reboot.
   A failed attempt (3 disconnects or 30 s) is shown on the page and the LCD.
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
| Output mode | Charger | Charger / PSU (regulated output voltage, battery optional) | menu, HomeKit custom |
| MPPT algorithm | on | on = MPPT, off = CC-CV only | menu, HomeKit custom |
| Battery preset | None | see 3.2; the user selects the type after first power-up | menu |
| Battery max voltage / Output voltage (PSU) | 27.30 V (FUGU) | 0–35 V (`vOutSystemMax`), 0.1 V step in the menu | menu, HomeKit custom |
| Battery min voltage | 22.40 V (FUGU) | 0 V – battery max | menu, HomeKit custom |
| Charging current | 30.0 A | 0–30 A | menu, HomeKit custom |
| Fan enabled | on | on/off | menu, HomeKit Fan |
| Fan on temperature | 60 °C | 0–100 °C | menu |
| Shutdown temperature | 90 °C | 0–120 °C | menu |
| Wi-Fi enabled | off | on/off; hidden in the menu while HAP is on (HAP keeps Wi-Fi on) | menu |
| HAP (HomeKit) enabled | off | on/off, requires Wi-Fi (turns it on); change → confirm → reboot | menu |
| LCD backlight | on | on/off | menu, HomeKit custom "Display" |
| LCD backlight sleep | 10 s (backlight off at start; the first key only lights it and does nothing else; stays on during a firmware download) | never / 10 s / 5 min / 1 h / 6 h / 12 h / 1 d / 3 d / 1 w / 1 mo | menu |
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

Default preset is **None**. Hardware limit `vOutSystemMax` = 35 V (`CONFIG_MPPT_CAL_VOUT_SYS_MAX_MV`):
the output capacitors C13/C14 on MPPT32 v1.1 are rated 35 V. FUGU uses 50 V; that value
returns once C13/C14 are 63 V parts. 63 V is the intended rating for C13/C14; the
ceramic output capacitors are rated 100 V. 48 V systems are not offered. Preset voltages confirmed.

Defined states (no undefined combination):

| Output mode | Preset | Behaviour |
|---|---|---|
| Charger | None | power stage off, LCD shows "Select battery type" until a preset or Custom is chosen |
| Charger | preset / Custom | FUGU charger algorithm (MPPT or CC-CV) with the preset voltages and current |
| PSU | any (None allowed) | regulated output at "Output voltage" for a load without battery; on input under-voltage the stage stops softly (see "Stopping the power stage" below), then stays disabled until Vin exceeds the set voltage plus dropout plus hysteresis for the recovery time |

Stopping the power stage. The IR2104 has one input: IN = 1 turns on the high-side MOSFET,
IN = 0 the low-side one, and `SD#` = 0 turns off both. A duty of 0 with the driver enabled
therefore keeps the low-side MOSFET on and shorts the output through the inductor; the
firmware never leaves the driver enabled at zero duty.

- **Immediate stop** (faults: over-current, short, over-voltage, over-temperature, ADS1115
  read failures; charger mode always): `SDWN` = 0 first, then PWM = 0, then backflow off.
  Both MOSFETs are off at once and the inductor current decays through the low-side body
  diode. FUGU's `buck_Disable()` uses the same order.
- **Soft stop** (PSU mode, planned stops: menu, OTA, input under-voltage): the output
  voltage set point is lowered, not the duty directly, so the loop reduces the duty only as
  fast as the output really falls. It ends with `SDWN` = 0 when the first of these is reached:
  - the output is at `CONFIG_MPPT_PSU_SOFTSTOP_LEVEL_PCT` of the set voltage (60–100 %,
    default 60 %: downstream regulators drop out around there anyway);
  - `CONFIG_MPPT_PSU_SOFTSTOP_MAX_MS` has passed (20–500 ms, default 150 ms);
  - the output current is below `CONFIG_MPPT_PSU_SOFTSTOP_MIN_CURRENT_MA` (100–1000 mA,
    default 200 mA). The output can only fall as fast as the load discharges the capacitors
    (dV/dt ≈ I_load / C_out); lowering the duty faster makes the synchronous buck drive
    current backwards, and with no load there is nothing to stop softly.

Menu editing steps: voltages 0.1 V, current 0.5 A, temperatures 1 °C, price 0.01. Short press
one step, hold auto-repeat.

### 3.3 Compile-time parameters (Kconfig, `menuconfig → MPPT`)

- All GPIO numbers from 1.1.
- I2C: bus 0/1 pins, frequency (400 kHz), ADS1115 address (0x48), ADS1115 channel assignment
  (Vin = 3, Vout = 0, current = 2, temperature = 1), PGA (±4.096 V), data rate (860 SPS),
  use of RDY pin.
- LCD: PCF8574 address (0x27; PCF8574A boards answer at 0x3F, and the firmware probes
  0x20–0x27 and 0x38–0x3F when the configured address does not answer), pin mapping
  (RS = P0, RW = P1, E = P2, BL = P3, D4–D7 = P4–P7).
- Calibration: `inVoltageDivRatio` 40.2156, `outVoltageDivRatio` 24.5, current scale 1.33,
  `currentSensV` 0.066 V/A, `currentMidPoint` 2.525 V, averaging counts (3 / 4 / 500),
  `voltageDropout` 1.0 V, `voltageBatteryThresh` 1.5 V, `currentInAbsolute` 31 A,
  `currentOutAbsolute` 50 A, `vInSystemMin` 10 V, `vOutSystemMax` 35 V (FUGU 50 V), `PPWM_margin` 99.5 %,
  `PWM_MaxDC` 97 %, `efficiencyRate` 1.0.
- PWM: frequency 39 kHz, resolution 11 bit (LEDC low-speed mode), P&O step 1.
- NTC: R25, B, R_fixed, Vdd, circuit mode, TH2 ADC channel.
- Timing: routine interval 250 ms, LCD interval 1 s, HAP push interval 2 s, error window 1 s,
  error count limit 5, menu timeout 7 s, Wh persist interval.
- HomeKit: manufacturer, model, hardware revision, setup code (test builds only), name pattern.
- Console: USB CDC on the mini-USB port (default; flashing and monitor work through it), or UART0 on T6 selected in menuconfig. The ESP32-S2 has one console, not both.

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

Main menu (values shown on the same line, e.g. `>4 BatMax   27.3V`): 1 Battery (preset),
2 OutMode (Chrg/PSU), 3 Algo (MPPT/CC-CV), 4 BatMax or OutVolt, 5 BatMin, 6 Current,
7 Charging, 8 Fan, 9 FanTemp, 10 MaxTemp, 11 Backlt, 12 BLsleep, 13 CntRst (auto reset
period), 14 Price, 15 Telem (0–3), 16 Reset counters, 17 Device Setup, 18 Exit. Settings are
saved when an edit is confirmed. FUGU's separate save/autoload items are not needed: every
confirmed edit is persisted. The **Device Setup** sub-menu:

| # | Item | Behaviour |
|---|---|---|
| 1 | Enable HAP / Disable HAP | label reflects the current state; turning on also turns Wi-Fi on; confirm → reboot |
| 2 | Enable WiFi / Disable WiFi | shown only while HAP is off; confirm → reboot |
| 3 | FW Update | checks `otafw`; "up to date" or the update prompt (section 6) |
| 4 | Reset WiFi | erases Wi-Fi credentials only (HAP pairing kept) → reboot into the portal |
| 5 | HAP Reset | shown only while HAP is on; removes HomeKit pairings only (Wi-Fi and settings kept) → reboot |
| 6 | Factory Reset | confirm → erase settings, counters, Wi-Fi credentials and HAP data → reboot |
| 7 | Info | firmware version, IP, RSSI, HAP pairing state |
| 8 | Exit | back to the main menu |

Every Wi-Fi / HAP change reboots the device; the LCD says so before confirming. Reset
overview: IO0 held 3 s = Reset WiFi; IO0 held 10 s, MENU held 10 s or menu item = Factory
Reset; HAP Reset only from the menu. With HAP off the HomeKit SDK is not initialised, so the
Wi-Fi and factory resets never call into it; the HAP data is erased through its NVS
namespaces instead.

---

## 5. HomeKit model

One accessory (category Switch), enabled only when the "HAP enabled" setting is on.

| Service | Characteristics | Source |
|---|---|---|
| Accessory Information | name `MPPT-xxxxxx`, manufacturer, model, serial = Wi-Fi MAC, firmware = `CONFIG_APP_PROJECT_VER`, hardware = 1.1 | Kconfig |
| Switch "Charger" | On = charging enabled | `chargingPause` inverted |
| ↳ custom (existing) | Display (LCD backlight) | existing template; the OTA trigger/status characteristics are dropped, OTA is driven from the LCD menu |
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

OTA through the **`general-ota`** component (`project/common/general-ota`, MIT): the
`esp_https_ota` streaming flow of `fupdateota` without the HomeKit coupling. Image URL
`https://raw.githubusercontent.com/AramVartanyan/otafw/master/mppt1hs2.bin`
(`CONFIG_GENERAL_OTA_URL`), server certificates from the mbedTLS bundle
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` in `sdkconfig.defaults`). The firmware version is
`CONFIG_APP_PROJECT_VER` (`CONFIG_APP_PROJECT_VER_FROM_CONFIG=y`), written by ESP-IDF into the
app descriptor; HomeKit reports it as Firmware Revision and the OTA check compares it
(`major.minor.patch`, only a strictly newer image is installed).

Versioning rule: every new pull request raises the patch number of `CONFIG_APP_PROJECT_VER` in
`project/mppt-hap/sdkconfig.defaults` by one (0.1.5 → 0.1.6), once the previous pull request
is merged into `main`. Further commits to a pull request that is still open keep its version.
An existing local `sdkconfig` keeps its own value; set it in menuconfig (Application manager →
Project version) or regenerate `sdkconfig`. Once more than one device is in use, a new image is
tested over USB on one device before it is uploaded to `otafw`.

What changed against `fupdateota` and why:

1. No `hap_get_paired_controller_count()` guard and no `FW_UPG_STATUS_*` values: with HAP
   disabled the old guard blocked OTA for good, and the status values existed only for the
   HomeKit characteristics of display-less devices. Here the LCD shows the state.
2. `general_ota_check()` fetches the image descriptor, compares and aborts without writing,
   so the menu shows "up to date" or "vX.Y.Z available" and the automatic check never
   downloads an image that is then rejected. `general_ota_update()` does check + download.
3. Events (`check start`, `up to date`, `update available`, `download start`, `progress`,
   `success`, `failed`) drive the LCD messages and the status LED; the application reboots
   after `success` (power stage stopped first).
4. `REQUIRES mbedtls` in the component, so `esp_crt_bundle.h` resolves on ESP-IDF 5.5.

Update policy: an automatic **check** runs 1 minute after Wi-Fi connects and then every
`CONFIG_MPPT_OTA_RECHECK_DAYS` (default 7 days, 0 = only after connect). When a newer image
exists the LCD shows `Update Firmware?  Yes / No` with **No** selected; the prompt stays until
the user answers (UP/DOWN moves, MENU confirms). Yes → power stage off → download with
progress → reboot. No → not asked again until the next automatic check (a week later), the
next boot, or a newer version.
Installation is never started without this confirmation, because the update reboots the
device and cuts the output while the load may be powered. Device Setup → FW Update runs the
same check and prompt on demand (for devices that have not rebooted for a long time).

---

## 7. Implementation phases

Each phase is reviewed and approved before the next starts.

1. **Infrastructure** (done) — `esp32s2` target, `sdkconfig.defaults`, Kconfig with MPPT32
   pins, CMake and `idf_component.yml`, ported `ads1115`, module skeletons with the module
   APIs, `captive-wifi` STA part, `app_main` boot order. Builds; buck disabled.
2. **Measurement and UI** (done, hardware test pending) — `mppt_sensors` port, LCD pages,
   numbered menu with Device Setup, settings editing, counter persistence, OTA prompt and
   automatic check, reset semantics in `mppt_system`, TH2 gated by Wi-Fi. To be tested from
   USB power without the power stage.
3. **Control** — `mppt_control`: protection, regulation and charging, in steps from simple
   to complex; each step is tested on the hardware before the next starts. Bench equipment:
   analog laboratory PSU, two channels of 20 V / 3 A with an adjustable, instant current limit
   (in series 40 V / 3 A, 120 W), small oscilloscope, 4 Ω / 500 W resistive load. With 40 V in
   and 4 Ω out, outputs up to about 20 V (5 A, 100 W) stay within the PSU's 120 W. In current
   limit the PSU behaves roughly like a PV panel (constant voltage, then constant current), so
   P&O MPPT can be tried on the bench before real panels.
   1. Inputs and outputs without switching: PWR12, fan by temperature with hysteresis;
      backflow and buck stay off.
   2. Protections dry run: all fault flags computed and shown, including the latch (2.5,
      item 8); thresholds may be lowered temporarily to trigger them. Buck stays off.
   3. Shutdown path: `SDWN` first, then PWM = 0; ADS1115 read failures count as a fault.
      Checked with the oscilloscope.
   4. Fixed duty cycle from the laboratory PSU into the resistive load: gate drive, inductor
      ripple (L8 estimate), current and voltage calibration against a multimeter.
   5. Output voltage regulation (the core of PSU mode) from the laboratory PSU: steady output
      under load changes, current limit.
   6. MPPT (P&O) from panels into the resistive load.
   7. Battery charging (CC-CV), only once a battery is used.
   8. PSU mode from panels: clouds, load steps, sunset ramp-down and recovery. The most
      testing and debugging.
4. **Connectivity** — `captive-wifi` component, Device Setup menu, OTA check; then
   `mppt_hap` and replacement of the template callbacks.
5. **Extras** — web status page on the device IP, TH2 redundancy, Wh persistence tuning, Eve
   characteristics.

---

## 8. Open items

- L8 parameters (see 1.5).
- ACS712 midpoint auto-calibration: kept exactly as in FUGU for now (whenever the buck is off
  and no FLV/OOV); a plausibility guard (PGOOD high, sensor voltage 2.0–3.0 V) is under
  consideration.
- HomeKit Fan service and custom characteristics: final configuration after testing with
  Apple's tools.
- Console: USB CDC on the mini-USB port, so the unit can be flashed and monitored when closed.

---

## 9. Licence and credits

Firmware: MIT (see `LICENSE`). Charging algorithm, protection logic and calibration constants
derived from FUGU-ARDUINO-MPPT-FIRMWARE by Angelo Casimiro (TechBuilder), released under
CC0 1.0. ADS1115 component by Blake Felt (Molorius). HD44780 driver by esp-idf-lib.
