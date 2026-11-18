# Thingy:91 X air monitor firmware

nRF Connect SDK (Zephyr) application for the **Thingy:91 X (nRF9151)** with two sensor builds:

| Build | Sensor | Measures | Alarm |
|---|---|---|---|
| **Default (demo)** | **ams iAQ-Core** on the Qwiic port (I2C) | CO2-equivalent (ppm), TVOC-equivalent (ppb) | **High** air-quality values |
| O2 (later) | Analog O2 cell via ADS1115 | O2 % | **Low** O2 |

Both builds include GNSS location, BME688 pressure/temperature, a local LED alarm that works without network, and **MQTT over TLS via LTE-M / NB-IoT** to your Linode server.

> This code has not been compiled here. It was written against the current nRF Connect SDK APIs and syntax-checked, so expect to fix a small compile error or two depending on your SDK version.

---

# Part A: iAQ-Core demo (default build)

## A1. What the iAQ-Core measures

The iAQ-Core is a metal-oxide **VOC** sensor. Its CO2 value is an **equivalent estimated from VOCs**, not a true CO2 measurement, and both outputs are relative (datasheet range 450–2000 ppm CO2-eq, 125–600 ppb TVOC-eq; higher values are still reported). It's good for showing air quality and ventilation in a demo, but it does **not** measure oxygen and is not a safety device.

It **calibrates itself** (automatic baseline correction), so there is no user calibration in this build.

## A2. Wiring (STEMMA QT / Qwiic port)

1. **Close solder bridges SB8 (SCL) and SB9 (SDA)** on the Thingy:91 X if they are open.
2. Connect the iAQ-Core to the Qwiic port (JST-SH 4-pin: GND, 3V3, SDA, SCL):

| iAQ-Core pin | Connect to |
|---|---|
| 2 SCL | Qwiic SCL |
| 3 GND | Qwiic GND |
| 4 SDA | Qwiic SDA |
| 6 VCC | Qwiic 3.3 V |
| 1, 5 | not connected |

Datasheet requirements to check:
- **Supply 3.3 V ±0.1 V, max 20 mV ripple, up to 20 mA.** The Qwiic port's supply comes from `VDD_EXP_BRD`; confirm on the schematic that it's 3.3 V and can supply 20 mA.
- **Pull-ups:** the TXS0102 level shifter has built-in 10 kΩ pull-ups. That's fine for short cables at 100 kHz, while the datasheet example uses 4.7 kΩ. If communication is unreliable, add 4.7 kΩ pull-ups to 3.3 V on the sensor side.
- **100 kHz and clock stretching:** the Thingy's I2C bus already runs at 100 kHz, and the nRF9151 supports clock stretching.
- **Never write to it.** The datasheet warns that addressing the iAQ-Core with the write bit can make it stop responding. The driver only reads (`i2c_read_dt`), and nothing else on the bus uses address 0x5A.

## A3. Choose the right version

| Version | Power | Reading every | Setting |
|---|---|---|---|
| **iAQ-Core P** (pulsed) | max 9 mW | 11 s | `CONFIG_IAQ_READ_INTERVAL_S=11` (default) |
| iAQ-Core C (continuous) | max 66 mW (20 mA) | 1 s | can use 1–11 s |

On battery, use the **P** version. The C version draws about 20 mA continuously and drains the battery in a few days.

## A4. Behaviour

- **Warm-up:** the first valid reading arrives about **5 minutes** after power-on. The LED **blinks blue** and the dashboard shows "Warming up". Metal-oxide sensors also keep stabilising over the first day or two of use.
- Readings are sent every **30 s**, immediately when the alarm changes, and when you **press the button**.
- **Alarm** when CO2-eq ≥ **1400 ppm** or TVOC-eq ≥ **450 ppb**. It clears when both fall below 1200 ppm and 350 ppb. Change these in `prj.conf` (`CONFIG_IAQ_*`) **and** at the top of the "Air quality alarm logic" node in Node-RED.
- **Sensor error:** a single ERROR reading is ignored. After 5 in a row the board reports a fault (red blinking LED, "SENSOR ERROR" on the dashboard).

Payload on `sensors/thingy91x/iaq`:

```json
{"co2":612,"tvoc":143,"res":256431,"status":"ok","alarm":false,"p":83.12,"t":24.1,"lat":-26.107600,"lon":28.056700,"acc":8}
{"status":"warmup","alarm":false,"p":83.12,"t":24.1}
```

## A5. Build and flash

1. Copy `secrets.conf.example` to `secrets.conf` and put in the **thingy91x** MQTT password.
2. Build:

```
west build -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE=secrets.conf
```

3. Flash as described in **B4** below.

## A6. LEDs

| LED | Meaning |
|---|---|
| Blue blinking | Sensor warming up |
| Red solid | **Air quality alarm** |
| Red blinking | Sensor fault (not found, or constant errors) |
| Yellow blinking | Not connected to the server yet |
| Green flash | Reading sent |

---

# Part B: O2 sensor (later stage)

Build with the O2 configuration and an ADC overlay:

```
west build -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="secrets.conf;o2.conf" -DEXTRA_DTC_OVERLAY_FILE=overlays/ads1115.overlay
```

## B1. Hardware

### Use the Thingy's STEMMA QT / Qwiic port with an ADS1115

The Thingy:91 X has a **STEMMA QT / Qwiic expansion connector** (4-pin, 1 mm pitch JST-SH). From the schematic:

- The nRF9151's internal I2C bus (SCL P0.08, SDA P0.09) reaches this connector through solder bridges **SB8 (SCL)** and **SB9 (SDA)**, then through a **TXS0102 level shifter (U24)**. The nRF side runs at 1.8 V; the shifter converts to the connector voltage (`VDD_EXP_BRD`).
- The same connector lines also go to nRF9151 pins **P0.18 / P0.19** (EXP_BOARD PIN1/PIN2, test points TP32/TP33).

So the cleanest setup is **no soldering on signal lines, just a cable**:

1. **Close SB8 and SB9** if they are open, with a small blob of solder. Check the PCB, or the notes in Nordic's hardware guide, for their default state.
2. Plug in an **ADS1115 breakout with a STEMMA QT / Qwiic connector**, using a JST-SH 4-pin cable.
3. Wire the O2 cell to the ADS1115:

| O2 cell | ADS1115 |
|---|---|
| **+** | **A0** |
| **−** | **A1**, and to **GND** |

Leave ADDR at its default (GND = 0x48).

**Galvanic cells need a load resistor.** Most raw O2 cells must have a load resistor across their terminals, as given in the datasheet (often 47 Ω–10 kΩ). Modules usually include it; a bare cell does not.

**Check `VDD_EXP_BRD`.** The connector's supply comes from `VDD_EXP_BRD`, and its presence also enables the level shifter (OE pin via R5). Look in the schematic for where that rail comes from. If it's switched by the power-management chip (nPM1300), the firmware must turn it on. Send me that part of the schematic and I'll add it.

Grove or DFRobot Gravity boards need an adapter cable, because those use 2 mm pitch connectors (as noted on the schematic).

### Why not the built-in ADC on TP32/TP33?

P0.18/P0.19 are analog-capable (AIN5/AIN6), but on this board they're wired to the level shifter. Its internal pull-up resistors and edge accelerators would load a millivolt-level sensor signal and give wrong readings. `overlays/saadc.overlay` is kept only as an option for an **amplified** sensor, used with SB8/SB9 open and nothing plugged into the port.

## B2. Fill in your sensor's datasheet values

Add these to `prj.conf` (values in **microvolts**, 1 mV = 1000 µV):

```
CONFIG_O2_AIR_NOMINAL_UV=11000   # typical output in air, e.g. "9-13 mV" -> 11000
CONFIG_O2_AIR_MIN_UV=9000        # lowest acceptable output in air
CONFIG_O2_AIR_MAX_UV=13000       # highest acceptable output in air
CONFIG_O2_ZERO_OFFSET_UV=0       # output at 0 % O2 (usually < 0.6 mV, often 0)
CONFIG_O2_ZERO_MAX_UV=600        # maximum zero offset allowed by the datasheet
CONFIG_O2_TEMP_COEFF_PPM_C=0     # only if the sensor is NOT temperature compensated
```

The nominal value is only used until you calibrate. A calibration outside the min/max window is **rejected**, which protects against calibrating a dead or disconnected sensor.

Other settings, with defaults, are in `Kconfig`: alarm levels, publish interval (30 s), GNSS interval (4 h), and others.

## B3. Build

Requires the **nRF Connect SDK** (latest release), installed through nRF Connect for VS Code or the `nrfutil` toolchain manager.

1. Copy `secrets.conf.example` to `secrets.conf` and put in the **thingy91x** MQTT password. `secrets.conf` is git-ignored.
2. Build:

```
west build -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE="secrets.conf;o2.conf" -DEXTRA_DTC_OVERLAY_FILE=overlays/ads1115.overlay
```

In VS Code: *Add build configuration* → board `thingy91x/nrf9151/ns` → *Extra Kconfig fragments*: `secrets.conf` (plus `o2.conf` for O2) → *Extra Devicetree overlays*: `overlays/ads1115.overlay` (O2 only).

## B4. Flash

The easiest way is the **nRF Connect for Desktop → Programmer** app over USB:

1. Switch the Thingy:91 X **off**, hold the **button**, and switch it **on** while holding. This enters the bootloader.
2. In Programmer, select the Thingy:91 X, add `build/dfu_application.zip`, and click **Write**.

With an external debug probe (for example an nRF9151 DK connected to the Thingy's debug connector), use `west flash` instead.

Logs appear on the Thingy:91 X's USB serial port at 115200 baud. Use the Serial Terminal app or any terminal program.

## B5. Calibration

All calibration values are stored in flash and survive power cycles and firmware updates.

### Conversion used

```
O2 % = span_pct x (V - V_zero) / (V_span - V_zero)          two-point linear
       x P_cal / P_now                                         pressure compensation (BME688)
       / (1 + k x (T_now - T_cal))                             optional temperature compensation
```

Until the sensor is calibrated, `V_zero` and `V_span` come from the datasheet values in `prj.conf`.

### Calibration options

| Option | How | When to use |
|---|---|---|
| **Air (20.9 %)** | Hold the button **3 s** and release when the LED turns blue, or use the dashboard button | Normal routine calibration ("zeroing" at 20.9 %) |
| **Span gas** | Dashboard: enter the gas % (5–100) and click Span calibrate | With a certified calibration gas, e.g. 18.0 % O2 in N2 |
| **Zero (0 %)** | Dashboard: Zero (N2) | Two-point calibration with pure nitrogen. Improves accuracy at low O2 levels |
| **Manual values** | Dashboard: Enter values manually | Values from a lab calibration certificate |
| **Reset** | Hold the button **10 s** (LED turns purple) and release, or use the dashboard button | Back to datasheet values, e.g. after replacing the sensor |

For a full two-point calibration, do the **zero first, then air or span**.

### Safety checks (calibration is rejected if any fail)

- **Stability:** during the second half of the measurement the reading must not move more than `CONFIG_O2_CAL_STABLE_UV` (200 µV). If it does, the sensor hasn't settled, so wait longer and retry.
- **Datasheet window:** the output must fit `CONFIG_O2_AIR_MIN_UV`..`MAX_UV` (converted to air), and a zero must be below `CONFIG_O2_ZERO_MAX_UV`. This prevents calibrating a dead, disconnected or wrongly gassed sensor.
- **Span gas range:** 5–100 % O2.

### Calibration due and sensor health

- **Calibration due** after `CONFIG_O2_CAL_INTERVAL_DAYS` (30 days), or if never calibrated. It shows on the dashboard and in every reading (`"cal_due":true`). Dates come from LTE network time.
- **Sensor health** = sensor output compared to the datasheet nominal of a new sensor. Galvanic cells lose output as they age. Below `CONFIG_O2_HEALTH_WARN_PCT` (70 %) the dashboard warns you to replace the sensor.

### Pressure and temperature compensation

Galvanic O2 cells measure oxygen **partial pressure**. The board records the barometric pressure (BME688) at calibration and scales readings by the pressure change since then. Without this, normal weather changes cause errors of a few tenths of a percent, and so would moving the sensor to a different altitude after calibrating.

Temperature compensation is **off** by default, because most O2 cells compensate internally. If yours doesn't, set `CONFIG_O2_TEMP_COEFF_PPM_C` from the datasheet (e.g. 2.5 %/°C = 25000). The BME688 is inside the Thingy enclosure, so it reads somewhat warmer than the air around the sensor.

### How to do an air calibration

1. Power on and let the sensor **warm up and stabilise**, at least 10–15 minutes or what the datasheet says.
2. Put it in **clean, fresh outdoor air**, away from people breathing on it and from gas cylinders.
3. Hold the button 3 s (or click **Fresh air** on the dashboard). The LED blinks blue for about 20 s.
4. **3 green blinks** = saved. **3 red blinks** = rejected; the dashboard shows why.

### Calibration commands (MQTT)

The dashboard sends these to `sensors/thingy91x/cmd`. Results come back on `sensors/thingy91x/status`.

```json
{"cmd":"cal_air"}
{"cmd":"cal_span","pct":18.0}
{"cmd":"cal_zero"}
{"cmd":"cal_set","zero_uv":0,"span_uv":10500,"pct":20.9}
{"cmd":"cal_reset"}
{"cmd":"cal_info"}
```

## B6. LED meaning

| LED | Meaning |
|---|---|
| Blue | Calibrating, or button held 3 s (release = air calibration) |
| Purple | Button held 10 s (release = reset calibration) |
| Red solid | **Low O2 alarm** |
| Red blinking | Sensor fault (ADC not found, or sensor output near zero) |
| Yellow blinking | Not connected to the server yet (LTE or MQTT) |
| Green flash | Reading sent |

A disconnected sensor reads close to 0 % O2, so it also triggers the alarm on the server. A failure shows as an alarm rather than as "everything is fine".

## C1. GNSS notes (both builds)

- GNSS needs a **view of the sky**. Indoors, in a basement or in a metal box it may never get a fix. The last known location stays on the map.
- Without assistance data, the first fix can take a few minutes outdoors (timeout: 5 minutes).
- `CONFIG_O2_GNSS_INTERVAL_S=0` means one fix at start-up only. This suits a fixed installation and saves battery.

## C2. Troubleshooting (watch the serial log)

| Log message | Likely cause |
|---|---|
| `iAQ-Core not responding at 0x5a` | SB8/SB9 open, no 3.3 V on the Qwiic port, cable, or wrong pins |
| `iAQ-Core status: error` (constant) | Defective sensor element |
| `O2 ADC not ready` | ADS1115 option: wiring, power, or I2C address (ADDR pin) |
| `DNS lookup ... failed` | No LTE data yet, or wrong hostname |
| `mqtt_connect failed` | TLS problem: certificate not provisioned, wrong hostname, or port 8883 blocked |
| `MQTT connection refused (5)` | Wrong username or password |
| `Rejected: ... outside the datasheet window` | Sensor output outside `CONFIG_O2_AIR_MIN_UV..MAX_UV` |
| `Not stable: reading moved ...` | Sensor not settled yet; wait and retry |
| `BME688 not available` | Pressure compensation disabled; the board still works |
| `GNSS: no fix within 300 s` | No sky view; move near a window or outdoors |

On the server, `docker compose logs -f mosquitto` shows the board connecting as `thingy91x-<IMEI>`.

---

# Location: Wi-Fi + GNSS + cellular (nRF Cloud)

The firmware gets a position every `CONFIG_O2_GNSS_INTERVAL_S` (10 min), trying in order:

1. **Wi-Fi**: the nRF7002 scans nearby networks (no connection, no passwords). Works indoors, about 10–50 m.
2. **GNSS**: needs open sky. Sped up by A-GNSS data from nRF Cloud.
3. **Cellular**: nearby cell towers. Always available, but only roughly accurate (hundreds of metres to km).

Set `CONFIG_APP_LOCATION_WIFI_FIRST=n` to try GNSS first (best for outdoor use).

The Wi-Fi and cell lookups and the A-GNSS data come from the **nRF Cloud location service** over CoAP. Sensor readings still go only to your own MQTT server. Requirements:
- The Thingy must be **registered in your nRF Cloud account** (Quick Start does this). Check at nrfcloud.com → Devices.
- Each lookup counts against your nRF Cloud plan's monthly location requests; at one every 10 minutes that's about 4,300 per month. Check your plan's limits.

Each message now includes `"loc":"wifi"|"gnss"|"cell"`, plus `lat`, `lon`, `acc` (m) and `fix_age` (s).

Serial log lines to look for:
- `Connected to nRF Cloud (location service)`
- `Location (wifi): -26.xxxxxx, 28.xxxxxx (+/- 30 m)`
- `nRF Cloud connect failed ...`: device not registered, or no credentials on the modem.

## Manual location (for demos)

Type in the serial terminal (PuTTY / Serial Terminal, 115200 baud), then press Enter:

```
loc set -26.107600 28.056700
loc show
loc clear
```

- `loc set` uses that position right away, saves it in flash (kept after power-off), and overrides GNSS/Wi-Fi/cell.
- `loc clear` goes back to automatic location.
- Coordinates: right-click the spot in Google Maps; the first menu line shows them.

Messages then carry `"loc":"manual"`. Optional defaults in `prj.conf`: `CONFIG_APP_FIXED_LAT`, `CONFIG_APP_FIXED_LON`, and `CONFIG_APP_FIXED_LOCATION_ALWAYS=y` to switch automatic location off entirely.
