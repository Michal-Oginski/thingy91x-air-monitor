#ifndef O2_SENSOR_H_
#define O2_SENSOR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int o2_sensor_init(void);

/* Take one ADC sample and add it to the moving average. */
int o2_sensor_sample(void);

/* Latest pressure/temperature for compensation (call from the main loop). */
void o2_sensor_set_env(bool valid, float pressure_kpa, float temp_c);

struct o2_reading {
	float o2_pct;       /* oxygen in % vol (compensated) */
	float sensor_mv;    /* averaged raw sensor output in mV */
	bool calibrated;    /* span calibration stored (otherwise datasheet nominal) */
	bool cal_due;       /* never calibrated, or calibration older than the interval */
	bool fault;         /* output implausibly low (disconnected?) */
	bool env_valid;
	float pressure_kpa;
	float temp_c;
};
bool o2_sensor_get(struct o2_reading *out);

/* ---------- Calibration ---------- */

enum o2_cal_type {
	O2_CAL_AIR,   /* fresh air, 20.9 % O2 */
	O2_CAL_SPAN,  /* certified span gas, concentration given */
	O2_CAL_ZERO,  /* 0 % O2 (pure nitrogen) */
};

/*
 * Measure and store a calibration point. Blocks for the calibration duration.
 * progress_cb is called every sample (e.g. to blink an LED).
 * A human-readable result is written to msg.
 * Returns 0 on success, -ERANGE (outside datasheet limits), -EAGAIN (not stable),
 * -EINVAL (bad parameters) or -EIO (ADC errors).
 */
int o2_sensor_calibrate(enum o2_cal_type type, float gas_pct, void (*progress_cb)(void),
			char *msg, size_t msg_len);

/* Enter calibration values by hand (e.g. from a lab certificate). */
int o2_sensor_cal_set(int32_t zero_uv, int32_t span_uv, float span_pct, char *msg,
		      size_t msg_len);

/* Forget all calibration and go back to the datasheet values. */
int o2_sensor_cal_reset(void);

struct o2_cal_info {
	bool has_span;
	bool has_zero;
	int32_t zero_uv;
	int32_t span_uv;
	float span_pct;
	int64_t span_time;    /* unix seconds, 0 = unknown */
	int64_t zero_time;
	float p_cal_kpa;      /* 0 = not recorded */
	float t_cal_c;
	int health_pct;       /* output vs datasheet nominal, -1 = unknown */
	int cal_age_days;     /* -1 = unknown */
	bool cal_due;
};
void o2_sensor_get_cal_info(struct o2_cal_info *out);

#endif /* O2_SENSOR_H_ */
