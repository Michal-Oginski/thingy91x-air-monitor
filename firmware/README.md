# Thingy:91 X environment monitor firmware

Zephyr application for the nRF9151 application processor. It publishes
onboard environmental sensor data and device location over MQTT/TLS.

## Measurements

- BME688 temperature
- BME688 pressure
- BME688 humidity
- BME688 gas resistance
- GNSS, Wi-Fi or cellular location

This firmware does not use iAQ-Core or O2 sensor hardware.

## MQTT payload

```json
{"status":"ok","temperature":24.10,"pressure":101.25,"humidity":44.20,"gas_resistance":125430,"lat":52.198671,"lon":21.047436,"acc":12,"loc":"gnss","fix_age":30}
```

Default topic: `sensors/thingy91x/env`

## Platform and build configuration

- Processor: Thingy:91 X nRF9151 application processor.
- Board target: `thingy91x/nrf9151/ns`.
- SDK/toolchain: nRF Connect SDK v3.4.2; this version was used for the verified build.
- Modem firmware: no separate modem image is pinned by this project. Use a modem firmware version supported by the selected NCS release. The device previously reported `mfw_nrf91x1_2.0.4`.
- Sensors: the onboard BME688 is accessed through Zephyr's sensor API. No external sensor is required.
- Location: the nRF9151 application uses the location library; Wi-Fi scans use the onboard nRF7002. The firmware does not provide general Wi-Fi data connectivity.

## Build

Copy `secrets.conf.example` to `secrets.conf`, set the MQTT password and broker
settings, then build for the nRF9151 non-secure target:

```bash
west build -b thingy91x/nrf9151/ns -d build/thingy91x-env -- \
  -DEXTRA_CONF_FILE=secrets.conf
```

The board overlay enables the built-in BME688. No extra Devicetree overlay or
sensor fragment is required.

## Programming and hardware validation

Program the image for the nRF9151, not the nRF5340. Follow Nordic's
[Thingy:91 X build and programming guide](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/device_guides/thingy91x/thingy91x_building_programming.html);
select the nRF91 target with SW2 and use an external SWD probe.

After programming, check the nRF9151 serial log for `LTE connected`, then
`MQTT connected`. Confirm that `sensors/thingy91x/env` reaches Node-RED with
`status: "ok"` and the BME688 measurements. A successful build alone does not
verify sensors, cellular registration, or broker connectivity.

## Runtime

- Press the Thingy button to publish immediately.
- Default sample interval is 10 seconds and publish interval is 30 seconds.
  GNSS/location updates are requested every 600 seconds. LTE, location scans,
  and publishing affect battery life; adjust the intervals in Kconfig for
  your latency and power requirements.
- Set a manual location over serial:

```text
loc set 52.198671 21.047436
loc show
loc clear
```

- RGB LED:
  - red blink: BME688 unavailable
  - yellow blink: no MQTT connection
  - green flash: measurement published
