/*
 * Analog O2 sensor read through an ADS1115 on the Qwiic port (default) or the
 * nRF9151 built-in ADC - selected in the devicetree (see boards/ and overlays/).
 *
 * Two-point linear conversion:
 *
 *   O2 % = span_pct * (V - V_zero) / (V_span - V_zero)
 *
 *   V_zero  : output at 0 % O2  (zero calibration in N2, or datasheet offset)
 *   V_span  : output at span_pct (air calibration = 20.9 %, or a span gas)
 *             until calibrated, the datasheet nominal output in air is used
 *
 * Optional compensation:
 *   pressure    : O2 % *= P_cal / P_now                    (CONFIG_O2_PRESSURE_COMP)
 *   temperature : O2 % /= 1 + k * (T_now - T_cal)           (CONFIG_O2_TEMP_COEFF_PPM_C)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <date_time.h>

#include "o2_sensor.h"

LOG_MODULE_REGISTER(o2_sensor, LOG_LEVEL_INF);

#define O2_AIR_PCT  20.9f
#define CAL_VERSION 1

static const struct adc_dt_spec adc_ch = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* Stored in flash as one blob */
struct cal_data {
	uint8_t version;
	uint8_t has_span;
	uint8_t has_zero;
	uint8_t reserved;
	int32_t zero_uv;
	int32_t span_uv;
	float span_pct;
	float p_cal_kpa;
	float t_cal_c;
	int64_t span_time;
	int64_t zero_time;
};

static struct cal_data cal;

static int32_t samples_uv[CONFIG_O2_AVG_SAMPLES];
static uint8_t sample_count;
static uint8_t sample_idx;

static bool env_ok;
static float env_p_kpa;
static float env_t_c;

static void cal_defaults(void)
{
	cal = (struct cal_data){
		.version = CAL_VERSION,
		.zero_uv = CONFIG_O2_ZERO_OFFSET_UV,
		.span_uv = CONFIG_O2_AIR_NOMINAL_UV,
		.span_pct = O2_AIR_PCT,
	};
}

static int64_t now_unix_s(void)
{
	int64_t ms;

	if (date_time_now(&ms) == 0) {
		return ms / 1000;
	}
	return 0;
}

/* ---------------- Settings (flash) ---------------- */

static int o2_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "cal", &next) && !next) {
		struct cal_data tmp;

		if (len != sizeof(tmp) || read_cb(cb_arg, &tmp, sizeof(tmp)) != sizeof(tmp) ||
		    tmp.version != CAL_VERSION) {
			LOG_WRN("Stored calibration invalid, using datasheet values");
			return 0;
		}
		cal = tmp;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(o2, "o2", NULL, o2_settings_set, NULL, NULL);

static void cal_save(void)
{
	int err = settings_save_one("o2/cal", &cal, sizeof(cal));

	if (err) {
		LOG_WRN("Calibration not saved to flash (%d)", err);
	}
}

static void reset_average(void)
{
	sample_count = 0;
	sample_idx = 0;
}

/* ---------------- ADC ---------------- */

static int read_uv(int32_t *uv)
{
	int16_t buf;
	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};
	int err;

	static uint32_t reads;

	adc_sequence_init_dt(&adc_ch, &seq);

	/* nRF SAADC: offset calibration at start and every ~10 minutes
	 * (the ADS1115 driver ignores this flag)
	 */
	seq.calibrate = (reads++ % 600) == 0;

	err = adc_read_dt(&adc_ch, &seq);
	if (err) {
		LOG_ERR("ADC read failed: %d", err);
		return err;
	}

	int32_t val = buf;

	err = adc_raw_to_microvolts_dt(&adc_ch, &val);
	if (err) {
		return err;
	}

	*uv = val;
	return 0;
}

int o2_sensor_init(void)
{
	int err;

	if (!adc_is_ready_dt(&adc_ch)) {
		LOG_ERR("O2 ADC not ready (ADS1115: check wiring and I2C address)");
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&adc_ch);
	if (err) {
		LOG_ERR("ADC channel setup failed: %d", err);
		return err;
	}

	cal_defaults();
	err = settings_subsys_init();
	if (!err) {
		settings_load_subtree("o2");
	} else {
		LOG_WRN("Settings init failed (%d), calibration will not be saved", err);
	}

	LOG_INF("O2 sensor ready: zero %d uV%s, span %d uV = %.2f %%%s", cal.zero_uv,
		cal.has_zero ? " (calibrated)" : " (datasheet)", cal.span_uv,
		(double)cal.span_pct, cal.has_span ? " (calibrated)" : " (datasheet - please calibrate)");
	return 0;
}

int o2_sensor_sample(void)
{
	int32_t uv;
	int err = read_uv(&uv);

	if (err) {
		return err;
	}

	samples_uv[sample_idx] = uv;
	sample_idx = (sample_idx + 1) % CONFIG_O2_AVG_SAMPLES;
	if (sample_count < CONFIG_O2_AVG_SAMPLES) {
		sample_count++;
	}
	return 0;
}

void o2_sensor_set_env(bool valid, float pressure_kpa, float temp_c)
{
	env_ok = valid;
	env_p_kpa = pressure_kpa;
	env_t_c = temp_c;
}

static float uv_to_pct(float uv)
{
	float span = (float)(cal.span_uv - cal.zero_uv);
	float pct = cal.span_pct * (uv - (float)cal.zero_uv) / span;

#if defined(CONFIG_O2_PRESSURE_COMP)
	if (env_ok && cal.p_cal_kpa > 0.0f && env_p_kpa > 50.0f) {
		pct *= cal.p_cal_kpa / env_p_kpa;
	}
#endif

#if CONFIG_O2_TEMP_COEFF_PPM_C != 0
	if (env_ok && cal.p_cal_kpa > 0.0f) { /* T_cal is recorded together with P_cal */
		float k = (float)CONFIG_O2_TEMP_COEFF_PPM_C * 1e-6f;

		pct /= 1.0f + k * (env_t_c - cal.t_cal_c);
	}
#endif

	return pct < 0.0f ? 0.0f : pct;
}

/* Output in air implied by the current calibration (for health + limits) */
static float air_equiv_uv(int32_t zero_uv, int32_t span_uv, float span_pct)
{
	return (float)zero_uv + (float)(span_uv - zero_uv) * O2_AIR_PCT / span_pct;
}

void o2_sensor_get_cal_info(struct o2_cal_info *out)
{
	int64_t now = now_unix_s();

	out->has_span = cal.has_span;
	out->has_zero = cal.has_zero;
	out->zero_uv = cal.zero_uv;
	out->span_uv = cal.span_uv;
	out->span_pct = cal.span_pct;
	out->span_time = cal.span_time;
	out->zero_time = cal.zero_time;
	out->p_cal_kpa = cal.p_cal_kpa;
	out->t_cal_c = cal.t_cal_c;

	out->health_pct = -1;
	if (cal.has_span) {
		float nominal = (float)(CONFIG_O2_AIR_NOMINAL_UV - CONFIG_O2_ZERO_OFFSET_UV);
		float actual = air_equiv_uv(cal.zero_uv, cal.span_uv, cal.span_pct) -
			       (float)cal.zero_uv;

		out->health_pct = (int)(100.0f * actual / nominal + 0.5f);
	}

	out->cal_age_days = -1;
	if (cal.has_span && cal.span_time > 0 && now > 0) {
		out->cal_age_days = (int)((now - cal.span_time) / 86400);
	}

	out->cal_due = !cal.has_span;
#if CONFIG_O2_CAL_INTERVAL_DAYS > 0
	if (out->cal_age_days >= CONFIG_O2_CAL_INTERVAL_DAYS) {
		out->cal_due = true;
	}
#endif
}

bool o2_sensor_get(struct o2_reading *out)
{
	if (sample_count == 0) {
		return false;
	}

	int64_t sum = 0;

	for (int i = 0; i < sample_count; i++) {
		sum += samples_uv[i];
	}

	float avg_uv = (float)sum / sample_count;
	struct o2_cal_info info;

	o2_sensor_get_cal_info(&info);

	out->sensor_mv = avg_uv / 1000.0f;
	out->o2_pct = uv_to_pct(avg_uv);
	out->calibrated = cal.has_span;
	out->cal_due = info.cal_due;
	out->fault = avg_uv < CONFIG_O2_SENSOR_FAULT_UV;
	out->env_valid = env_ok;
	out->pressure_kpa = env_p_kpa;
	out->temp_c = env_t_c;
	return true;
}

/* ---------------- Calibration ---------------- */

/* Average over the whole window; stability judged on the second half */
static int measure(int duration_s, void (*cb)(void), int32_t *avg, int32_t *spread)
{
	const int n = (duration_s * 1000) / CONFIG_O2_SAMPLE_INTERVAL_MS;
	int64_t sum = 0;
	int good = 0;
	int32_t lo = INT32_MAX, hi = INT32_MIN;

	for (int i = 0; i < n; i++) {
		int32_t uv;

		if (read_uv(&uv) == 0) {
			sum += uv;
			good++;
			if (i >= n / 2) {
				lo = MIN(lo, uv);
				hi = MAX(hi, uv);
			}
		}
		if (cb) {
			cb();
		}
		k_msleep(CONFIG_O2_SAMPLE_INTERVAL_MS);
	}

	if (good < n / 2 || hi < lo) {
		return -EIO;
	}

	*avg = (int32_t)(sum / good);
	*spread = hi - lo;
	return 0;
}

int o2_sensor_calibrate(enum o2_cal_type type, float gas_pct, void (*progress_cb)(void),
			char *msg, size_t msg_len)
{
	int32_t avg, spread;
	int err;

	if (type == O2_CAL_AIR) {
		gas_pct = O2_AIR_PCT;
	} else if (type == O2_CAL_SPAN && (gas_pct < 5.0f || gas_pct > 100.0f)) {
		snprintf(msg, msg_len, "Span gas must be 5-100 %% O2 (got %.2f)", (double)gas_pct);
		return -EINVAL;
	}

	int duration = (type == O2_CAL_ZERO) ? CONFIG_O2_ZERO_CAL_DURATION_S
					     : CONFIG_O2_CAL_DURATION_S;

	LOG_INF("Calibration (%s) started, %d s",
		type == O2_CAL_AIR ? "air" : type == O2_CAL_SPAN ? "span gas" : "zero",
		duration);

	err = measure(duration, progress_cb, &avg, &spread);
	if (err) {
		snprintf(msg, msg_len, "Calibration failed: ADC errors");
		return err;
	}

	if (spread > CONFIG_O2_CAL_STABLE_UV) {
		snprintf(msg, msg_len,
			 "Not stable: reading moved %d uV (max %d). Wait longer and retry",
			 spread, CONFIG_O2_CAL_STABLE_UV);
		return -EAGAIN;
	}

	if (type == O2_CAL_ZERO) {
		if (abs(avg) > CONFIG_O2_ZERO_MAX_UV) {
			snprintf(msg, msg_len,
				 "Zero rejected: %d uV is above the datasheet max %d uV. Is the "
				 "sensor really in pure nitrogen?",
				 avg, CONFIG_O2_ZERO_MAX_UV);
			return -ERANGE;
		}
		float air = air_equiv_uv(avg, cal.span_uv, cal.span_pct);

		if (air < CONFIG_O2_AIR_MIN_UV || air > CONFIG_O2_AIR_MAX_UV) {
			snprintf(msg, msg_len,
				 "Zero rejected: does not fit the stored span calibration");
			return -ERANGE;
		}
		cal.zero_uv = avg;
		cal.has_zero = 1;
		cal.zero_time = now_unix_s();
		snprintf(msg, msg_len, "Zero calibration OK: %d uV = 0 %% O2", avg);
	} else {
		float air = air_equiv_uv(cal.zero_uv, avg, gas_pct);

		if (avg <= cal.zero_uv || air < CONFIG_O2_AIR_MIN_UV ||
		    air > CONFIG_O2_AIR_MAX_UV) {
			snprintf(msg, msg_len,
				 "Rejected: %d uV at %.2f %% is outside the datasheet window "
				 "(%d-%d uV in air)",
				 avg, (double)gas_pct, CONFIG_O2_AIR_MIN_UV, CONFIG_O2_AIR_MAX_UV);
			return -ERANGE;
		}
		cal.span_uv = avg;
		cal.span_pct = gas_pct;
		cal.has_span = 1;
		cal.span_time = now_unix_s();
		/* Remember conditions for pressure/temperature compensation */
		cal.p_cal_kpa = env_ok ? env_p_kpa : 0.0f;
		cal.t_cal_c = env_ok ? env_t_c : 0.0f;
		snprintf(msg, msg_len, "%s calibration OK: %d uV = %.2f %% O2%s",
			 type == O2_CAL_AIR ? "Air" : "Span gas", avg, (double)gas_pct,
			 env_ok ? "" : " (no pressure reference)");
	}

	cal_save();
	reset_average();
	LOG_INF("%s", msg);
	return 0;
}

int o2_sensor_cal_set(int32_t zero_uv, int32_t span_uv, float span_pct, char *msg,
		      size_t msg_len)
{
	if (span_pct < 5.0f || span_pct > 100.0f || abs(zero_uv) > CONFIG_O2_ZERO_MAX_UV ||
	    span_uv <= zero_uv) {
		snprintf(msg, msg_len, "Manual values rejected: check zero/span/pct");
		return -EINVAL;
	}

	float air = air_equiv_uv(zero_uv, span_uv, span_pct);

	if (air < CONFIG_O2_AIR_MIN_UV || air > CONFIG_O2_AIR_MAX_UV) {
		snprintf(msg, msg_len, "Manual values rejected: outside datasheet window");
		return -ERANGE;
	}

	int64_t now = now_unix_s();

	cal.zero_uv = zero_uv;
	cal.span_uv = span_uv;
	cal.span_pct = span_pct;
	cal.has_zero = 1;
	cal.has_span = 1;
	cal.zero_time = now;
	cal.span_time = now;
	cal.p_cal_kpa = 0.0f; /* unknown conditions: no pressure compensation */
	cal.t_cal_c = 0.0f;

	cal_save();
	reset_average();
	snprintf(msg, msg_len, "Manual calibration set: zero %d uV, %d uV = %.2f %%", zero_uv,
		 span_uv, (double)span_pct);
	LOG_INF("%s", msg);
	return 0;
}

int o2_sensor_cal_reset(void)
{
	cal_defaults();
	settings_delete("o2/cal");
	reset_average();
	LOG_INF("Calibration reset to datasheet values");
	return 0;
}
