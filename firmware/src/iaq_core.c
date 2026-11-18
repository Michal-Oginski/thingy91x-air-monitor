/*
 * ams iAQ-Core driver (read-only).
 *
 * IMPORTANT (datasheet): never address the iAQ-Core with the WRITE bit -
 * it can lose its configuration and stop responding. This driver only ever
 * uses i2c_read_dt(), which sends the address with the read bit (0xB5).
 *
 * Frame (9 bytes, MSB first):
 *   0-1 CO2-eq prediction (ppm)   2 status   3-6 resistance (Ohm)   7-8 TVOC-eq (ppb)
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "iaq_core.h"

LOG_MODULE_REGISTER(iaq_core, LOG_LEVEL_INF);

static const struct i2c_dt_spec iaq = I2C_DT_SPEC_GET(DT_NODELABEL(iaq_core));

static uint8_t last_status = 0xFF;
static int error_streak;

const char *iaq_status_str(uint8_t status)
{
	switch (status) {
	case IAQ_STATUS_OK:
		return "ok";
	case IAQ_STATUS_BUSY:
		return "busy";
	case IAQ_STATUS_RUNIN:
		return "warmup";
	case IAQ_STATUS_ERROR:
		return "error";
	case IAQ_STATUS_NO_SENSOR:
		return "no_sensor";
	default:
		return "unknown";
	}
}

int iaq_init(void)
{
	if (!i2c_is_ready_dt(&iaq)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	struct iaq_data d;
	int err = iaq_read(&d);

	if (err) {
		static bool reported;

		/* Report once, then keep retrying quietly */
		if (!reported) {
			LOG_ERR("iAQ-Core not responding at 0x%02x (%d) - check SB8/SB9, power, cable",
				iaq.addr, err);
			reported = true;
		}
		return err;
	}

	LOG_INF("iAQ-Core found (status: %s)", iaq_status_str(d.status));
	return 0;
}

int iaq_read(struct iaq_data *d)
{
	uint8_t b[9];
	int err;

	/* BUSY means multi-byte values may be half-updated: re-read */
	for (int attempt = 0; attempt < 3; attempt++) {
		err = i2c_read_dt(&iaq, b, sizeof(b));
		if (err) {
			return err;
		}
		if (b[2] != IAQ_STATUS_BUSY) {
			break;
		}
		k_msleep(100);
	}

	d->co2_ppm = ((uint16_t)b[0] << 8) | b[1];
	d->status = b[2];
	d->resistance_ohm = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];
	d->tvoc_ppb = ((uint16_t)b[7] << 8) | b[8];

	/* 0xFFFF = "no data" from the sensor: treat as busy, never as a reading */
	if (d->status == IAQ_STATUS_OK && (d->co2_ppm == 0xFFFF || d->tvoc_ppb == 0xFFFF)) {
		LOG_WRN("iAQ-Core returned invalid data (CO2 %u, TVOC %u) - ignored",
			d->co2_ppm, d->tvoc_ppb);
		d->status = IAQ_STATUS_BUSY;
	}

	/* A single ERROR can happen; a constant one means a defective sensor */
	if (d->status == IAQ_STATUS_ERROR) {
		if (++error_streak < CONFIG_IAQ_ERROR_COUNT) {
			d->status = IAQ_STATUS_BUSY; /* treat as "no new data" for now */
		}
	} else {
		error_streak = 0;
	}

	if (d->status != last_status) {
		LOG_INF("iAQ-Core status: %s", iaq_status_str(d->status));
		last_status = d->status;
	}
	return 0;
}
