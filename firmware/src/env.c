#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "env.h"

LOG_MODULE_REGISTER(env, LOG_LEVEL_INF);

static const struct device *bme = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(bme680));

int env_init(void)
{
	if (!bme || !device_is_ready(bme)) {
		LOG_WRN("BME688 not available - no pressure/temperature compensation");
		return -ENODEV;
	}
	return 0;
}

int env_read(struct env_reading *reading)
{
	struct sensor_value pressure, temperature, humidity, gas;
	int err;

	if (!bme || !device_is_ready(bme)) {
		return -ENODEV;
	}

	err = sensor_sample_fetch(bme);
	if (err) {
		return err;
	}

	err = sensor_channel_get(bme, SENSOR_CHAN_PRESS, &pressure);
	if (err) {
		LOG_ERR("Failed to read BME688 pressure: %d", err);
		return err;
	}

	err = sensor_channel_get(bme, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
	if (err) {
		LOG_ERR("Failed to read BME688 temperature: %d", err);
		return err;
	}

	reading->pressure_kpa = (float)sensor_value_to_double(&pressure);
	reading->temperature_c = (float)sensor_value_to_double(&temperature);
	reading->humidity_pct = 0.0f;
	reading->gas_resistance_ohm = 0.0f;

	if (sensor_channel_get(bme, SENSOR_CHAN_HUMIDITY, &humidity) == 0) {
		reading->humidity_pct = (float)sensor_value_to_double(&humidity);
	}
	if (sensor_channel_get(bme, SENSOR_CHAN_GAS_RES, &gas) == 0) {
		reading->gas_resistance_ohm = (float)sensor_value_to_double(&gas);
	}
	return 0;
}
