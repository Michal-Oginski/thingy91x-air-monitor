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

int env_read(float *pressure_kpa, float *temp_c)
{
	struct sensor_value p, t;
	int err;

	if (!bme || !device_is_ready(bme)) {
		return -ENODEV;
	}

	err = sensor_sample_fetch(bme);
	if (err) {
		return err;
	}

	sensor_channel_get(bme, SENSOR_CHAN_PRESS, &p);       /* kPa */
	sensor_channel_get(bme, SENSOR_CHAN_AMBIENT_TEMP, &t); /* degC */

	*pressure_kpa = (float)sensor_value_to_double(&p);
	*temp_c = (float)sensor_value_to_double(&t);
	return 0;
}
