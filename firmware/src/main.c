/*
 * Thingy:91 X air monitor
 *
 * Two sensor builds (Kconfig choice APP_SENSOR):
 *   APP_SENSOR_IAQ_CORE (default, demo): ams iAQ-Core on the Qwiic port
 *       -> CO2-equivalent (ppm) and TVOC-equivalent (ppb), high-level alarms
 *   APP_SENSOR_O2: analog O2 cell, low-O2 alarm, calibration (see o2.conf)
 *
 * --- O2 build ---
 *  - Reads an analog O2 sensor through an ADS1115 every second (moving average)
 *  - Local low-O2 alarm with hysteresis on the RGB LED (works without network)
 *  - Calibration (stored in flash):
 *      button held 3 s   -> air calibration (20.9 % O2)
 *      button held 10 s  -> reset to datasheet values
 *      MQTT commands on CONFIG_O2_MQTT_CMD_TOPIC (from the dashboard):
 *        {"cmd":"cal_air"}                       fresh air = 20.9 %
 *        {"cmd":"cal_span","pct":18.0}           certified span gas
 *        {"cmd":"cal_zero"}                      pure nitrogen = 0 %
 *        {"cmd":"cal_set","zero_uv":0,"span_uv":10500,"pct":20.9}   manual entry
 *        {"cmd":"cal_reset"}                     back to datasheet values
 *        {"cmd":"cal_info"}                      re-send calibration info
 *  - Pressure (and optional temperature) compensation using the built-in BME688
 *  - GNSS fix at start-up and every few hours
 *  - Publishes JSON over MQTT/TLS (LTE-M / NB-IoT) to your own broker:
 *      {"o2":20.85,"mv":10.412,"cal":true,"alarm":false,"fault":false,
 *       "cal_due":false,"p":83.12,"t":24.1,"lat":-26.107600,"lon":28.056700,"acc":8}
 *
 * LED:  blue             = calibrating (or button held 3 s)
 *       purple           = button held 10 s (release = reset calibration)
 *       red solid        = low O2 alarm
 *       red blinking     = sensor fault (disconnected / no ADC)
 *       yellow blinking  = not connected to the server
 *       green flash      = reading sent
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/sys/atomic.h>
#include <nrf_modem_at.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <dk_buttons_and_leds.h>

#if defined(CONFIG_APP_SENSOR_O2)
#include "o2_sensor.h"
#else
#include "iaq_core.h"
#endif
#include "gnss.h"
#include "manual_loc.h"
#include "env.h"

LOG_MODULE_REGISTER(o2_main, LOG_LEVEL_INF);

/* --- Compatibility with older/newer Zephyr networking names --- */
#if !defined(NET_AF_INET)
#define NET_AF_INET          AF_INET
#define NET_SOCK_STREAM      SOCK_STREAM
#define net_sockaddr_in      sockaddr_in
#define net_sockaddr_storage sockaddr_storage
#define net_htons            htons
#endif
#if !defined(ZSOCK_TLS_PEER_VERIFY_REQUIRED)
#define ZSOCK_TLS_PEER_VERIFY_REQUIRED   TLS_PEER_VERIFY_REQUIRED
#define ZSOCK_TLS_SESSION_CACHE_DISABLED TLS_SESSION_CACHE_DISABLED
#endif

#define LED_RED   DK_LED1
#define LED_GREEN DK_LED2
#define LED_BLUE  DK_LED3

#define RECONNECT_DELAY_MS (30 * MSEC_PER_SEC)

/* Mikrus MQTT broker CA, embedded at build time */
static const char ca_cert[] = {
#include "ca_cert.pem.inc"
	0x00
};

/* ---------------- State ---------------- */

static atomic_t lte_ready;
/* Requests from the button (handled in the main loop) */
enum { REQ_NONE, REQ_CAL_AIR, REQ_CAL_RESET, REQ_SEND_NOW };
static atomic_t button_req;
#if defined(CONFIG_APP_SENSOR_O2)
static int64_t press_time;
#endif
static atomic_t button_down;

/* Command received over MQTT (copied here, handled in the main loop) */
static char cmd_buf[256];
static atomic_t cmd_pending;

static struct mqtt_client client;
static struct net_sockaddr_storage broker;
static struct zsock_pollfd fds;
static bool mqtt_open;
static bool mqtt_connected;
static uint16_t msg_id;

static uint8_t rx_buf[256];
static uint8_t tx_buf[768];
static char client_id[48] = "thingy91x";

static bool alarm_active;
static bool sensor_ok;
static bool env_ok;
static struct gnss_fix pending_fix;
static bool fix_pending;   /* true once we have ANY fix; sent with every message */
static int64_t fix_time_ms; /* uptime of the last fix */
static bool warming_up;   /* iAQ-Core run-in: blue blink */

#if defined(CONFIG_APP_SENSOR_O2)
static void publish_cal_info(void);
#endif

#if defined(CONFIG_APP_SENSOR_O2)
/* ---------------- Button: hold 3 s = air cal, 10 s = reset ---------------- */

static void hold_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	dk_set_led(LED_BLUE, 1); /* release now -> air calibration */
}
static K_WORK_DELAYABLE_DEFINE(hold_work, hold_work_fn);

static void reset_hold_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	dk_set_led(LED_RED, 1); /* red + blue = purple: release -> reset */
	dk_set_led(LED_BLUE, 1);
}
static K_WORK_DELAYABLE_DEFINE(reset_hold_work, reset_hold_work_fn);

static void button_handler(uint32_t state, uint32_t changed)
{
	if (!(changed & DK_BTN1_MSK)) {
		return;
	}

	if (state & DK_BTN1_MSK) {
		atomic_set(&button_down, 1);
		press_time = k_uptime_get();
		k_work_schedule(&hold_work, K_MSEC(CONFIG_O2_CAL_HOLD_MS));
		k_work_schedule(&reset_hold_work, K_MSEC(CONFIG_O2_CAL_RESET_HOLD_MS));
		return;
	}

	int64_t held = k_uptime_get() - press_time;

	atomic_set(&button_down, 0);

	k_work_cancel_delayable(&hold_work);
	k_work_cancel_delayable(&reset_hold_work);

	if (held >= CONFIG_O2_CAL_RESET_HOLD_MS) {
		atomic_set(&button_req, REQ_CAL_RESET);
	} else if (held >= CONFIG_O2_CAL_HOLD_MS) {
		atomic_set(&button_req, REQ_CAL_AIR);
	}
}

#else /* iAQ-Core: press the button to send a reading now */

static void button_handler(uint32_t state, uint32_t changed)
{
	if ((changed & DK_BTN1_MSK) && (state & DK_BTN1_MSK)) {
		atomic_set(&button_req, REQ_SEND_NOW);
	}
}
#endif

/* ---------------- LTE ---------------- */

static void lte_handler(const struct lte_lc_evt *evt)
{
	if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {
		bool reg = evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
			   evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;

		if (reg != (bool)atomic_get(&lte_ready)) {
			LOG_INF("LTE %s", reg ? "connected" : "disconnected");
		}
		atomic_set(&lte_ready, reg);
	}
}

static int provision_ca_cert(void)
{
	bool exists = false;
	int err = modem_key_mgmt_exists(CONFIG_O2_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					&exists);

	if (err) {
		LOG_ERR("modem_key_mgmt_exists: %d", err);
		return err;
	}

	if (exists && modem_key_mgmt_cmp(CONFIG_O2_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					 ca_cert, strlen(ca_cert)) == 0) {
		return 0; /* already the right certificate */
	}

	if (exists) {
		modem_key_mgmt_delete(CONFIG_O2_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
	}

	LOG_INF("Writing CA certificate to sec tag %d", CONFIG_O2_SEC_TAG);
	return modem_key_mgmt_write(CONFIG_O2_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    ca_cert, strlen(ca_cert));
}

static void read_imei_client_id(void)
{
	char imei[32] = { 0 };

	if (nrf_modem_at_cmd(imei, sizeof(imei), "AT+CGSN") == 0) {
		imei[15] = '\0'; /* response is "<15 digits>\r\nOK" */
		snprintf(client_id, sizeof(client_id), "thingy91x-%s", imei);
	}
	LOG_INF("MQTT client ID: %s", client_id);
}

/* ---------------- MQTT ---------------- */

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			mqtt_connected = true;
			LOG_INF("MQTT connected to %s", CONFIG_O2_MQTT_HOST);
		} else {
			LOG_ERR("MQTT connection refused (%d) - check username/password",
				evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected (%d)", evt->result);
		mqtt_connected = false;
		mqtt_open = false;
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		size_t len = p->message.payload.len;
		char tmp[sizeof(cmd_buf)];
		size_t keep = MIN(len, sizeof(tmp) - 1);

		/* Read the payload (drain anything that does not fit) */
		if (mqtt_readall_publish_payload(c, (uint8_t *)tmp, keep) == 0) {
			size_t left = len - keep;

			while (left > 0) {
				char junk[32];
				size_t chunk = MIN(left, sizeof(junk));

				if (mqtt_readall_publish_payload(c, (uint8_t *)junk, chunk)) {
					break;
				}
				left -= chunk;
			}
			tmp[keep] = '\0';

			if (!atomic_get(&cmd_pending)) {
				memcpy(cmd_buf, tmp, keep + 1);
				atomic_set(&cmd_pending, 1);
			} else {
				LOG_WRN("Command ignored - previous one still running");
			}
		}

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = { .message_id = p->message_id };

			mqtt_publish_qos1_ack(c, &ack);
		}
		break;
	}
	case MQTT_EVT_PUBACK:
	case MQTT_EVT_SUBACK:
		break;
	default:
		break;
	}
}

static int broker_resolve(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = NET_AF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	int err = zsock_getaddrinfo(CONFIG_O2_MQTT_HOST, NULL, &hints, &res);

	if (err) {
		LOG_ERR("DNS lookup of %s failed: %d", CONFIG_O2_MQTT_HOST, err);
		return -EIO;
	}

	struct net_sockaddr_in *b4 = (struct net_sockaddr_in *)&broker;

	memcpy(b4, res->ai_addr, sizeof(*b4));
	b4->sin_family = NET_AF_INET;
	b4->sin_port = net_htons(CONFIG_O2_MQTT_PORT);
	zsock_freeaddrinfo(res);
	return 0;
}

static int mqtt_start(void)
{
	static sec_tag_t sec_tags[] = { CONFIG_O2_SEC_TAG };
	static struct mqtt_utf8 user = {
		.utf8 = (uint8_t *)CONFIG_O2_MQTT_USER,
		.size = sizeof(CONFIG_O2_MQTT_USER) - 1,
	};
	static struct mqtt_utf8 pass = {
		.utf8 = (uint8_t *)CONFIG_O2_MQTT_PASSWORD,
		.size = sizeof(CONFIG_O2_MQTT_PASSWORD) - 1,
	};
	int err = broker_resolve();

	if (err) {
		return err;
	}

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);
	client.user_name = &user;
	client.password = &pass;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.keepalive = CONFIG_O2_MQTT_KEEPALIVE_S;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);

	client.transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls = &client.transport.tls.config;

	tls->peer_verify = ZSOCK_TLS_PEER_VERIFY_REQUIRED;
	tls->cipher_list = NULL;
	tls->cipher_count = 0;
	tls->sec_tag_list = sec_tags;
	tls->sec_tag_count = ARRAY_SIZE(sec_tags);
	tls->hostname = CONFIG_O2_MQTT_HOST;
	tls->session_cache = ZSOCK_TLS_SESSION_CACHE_DISABLED;

	mqtt_connected = false;
	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect failed: %d (TLS/certificate or network problem)", err);
		return err;
	}

	mqtt_open = true;
	fds.fd = client.transport.tls.sock;
	fds.events = ZSOCK_POLLIN;

	/* Wait up to 10 s for CONNACK */
	for (int i = 0; i < 10 && mqtt_open && !mqtt_connected; i++) {
		if (zsock_poll(&fds, 1, MSEC_PER_SEC) > 0) {
			mqtt_input(&client);
		}
	}

	if (!mqtt_connected) {
		if (mqtt_open) {
			mqtt_abort(&client);
			mqtt_open = false;
		}
		return -ETIMEDOUT;
	}

#if defined(CONFIG_APP_SENSOR_O2)
	/* Listen for calibration commands */
	static const struct mqtt_topic cmd_topic = {
		.topic = {
			.utf8 = (uint8_t *)CONFIG_O2_MQTT_CMD_TOPIC,
			.size = sizeof(CONFIG_O2_MQTT_CMD_TOPIC) - 1,
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	const struct mqtt_subscription_list subs = {
		.list = &cmd_topic,
		.list_count = 1,
		.message_id = ++msg_id,
	};

	err = mqtt_subscribe(&client, &subs);
	if (err) {
		LOG_WRN("Subscribe to %s failed: %d", CONFIG_O2_MQTT_CMD_TOPIC, err);
	}

	publish_cal_info();
#endif
	return 0;
}

static void mqtt_service(int timeout_ms)
{
	if (!mqtt_open) {
		k_msleep(timeout_ms);
		return;
	}

	int ret = zsock_poll(&fds, 1, timeout_ms);

	if (ret > 0) {
		if (fds.revents & ZSOCK_POLLIN) {
			mqtt_input(&client);
		}
		if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
			LOG_WRN("MQTT socket closed");
			mqtt_abort(&client);
			mqtt_open = false;
			mqtt_connected = false;
		}
	}

	if (mqtt_open) {
		mqtt_live(&client);
	}
}

static int publish_raw(const char *topic, const char *payload, bool retain)
{
	struct mqtt_publish_param p = {
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.message.topic.topic.utf8 = (uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.payload.data = (uint8_t *)payload,
		.message.payload.len = strlen(payload),
		.message_id = ++msg_id,
		.retain_flag = retain ? 1 : 0,
	};

	if (!mqtt_connected) {
		return -ENOTCONN;
	}
	return mqtt_publish(&client, &p);
}

#if defined(CONFIG_APP_SENSOR_O2)
/* Calibration info, retained so the dashboard always has the latest */
static void publish_cal_info(void)
{
	struct o2_cal_info ci;
	char buf[400];

	o2_sensor_get_cal_info(&ci);
	snprintf(buf, sizeof(buf),
		 "{\"type\":\"cal_info\",\"has_span\":%s,\"has_zero\":%s,"
		 "\"zero_uv\":%d,\"span_uv\":%d,\"span_pct\":%.2f,"
		 "\"span_time\":%lld,\"zero_time\":%lld,\"p_cal\":%.2f,\"t_cal\":%.1f,"
		 "\"health\":%d,\"age_d\":%d,\"due\":%s,\"interval_d\":%d,"
		 "\"health_warn\":%d,\"p_comp\":%s,\"t_coeff_ppm\":%d}",
		 ci.has_span ? "true" : "false", ci.has_zero ? "true" : "false", ci.zero_uv,
		 ci.span_uv, (double)ci.span_pct, (long long)ci.span_time,
		 (long long)ci.zero_time, (double)ci.p_cal_kpa, (double)ci.t_cal_c,
		 ci.health_pct, ci.cal_age_days, ci.cal_due ? "true" : "false",
		 CONFIG_O2_CAL_INTERVAL_DAYS, CONFIG_O2_HEALTH_WARN_PCT,
		 IS_ENABLED(CONFIG_O2_PRESSURE_COMP) ? "true" : "false",
		 CONFIG_O2_TEMP_COEFF_PPM_C);

	if (publish_raw(CONFIG_O2_MQTT_STATUS_TOPIC, buf, true) == 0) {
		LOG_INF("Calibration info sent");
	}
}

static void publish_cal_result(const char *cmd, bool ok, const char *msg)
{
	char buf[300];

	snprintf(buf, sizeof(buf), "{\"type\":\"cal_result\",\"cmd\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}",
		 cmd, ok ? "true" : "false", msg);
	publish_raw(CONFIG_O2_MQTT_STATUS_TOPIC, buf, false);
}

static int publish_reading(const struct o2_reading *r)
{
	char payload[320];
	int len;

	len = snprintf(payload, sizeof(payload),
		       "{\"o2\":%.2f,\"mv\":%.3f,\"cal\":%s,\"alarm\":%s,\"fault\":%s",
		       (double)r->o2_pct, (double)r->sensor_mv, r->calibrated ? "true" : "false",
		       alarm_active ? "true" : "false", r->fault ? "true" : "false");

	len += snprintf(payload + len, sizeof(payload) - len, ",\"cal_due\":%s",
			r->cal_due ? "true" : "false");
	if (r->env_valid) {
		len += snprintf(payload + len, sizeof(payload) - len, ",\"p\":%.2f,\"t\":%.1f",
				(double)r->pressure_kpa, (double)r->temp_c);
	}

	if (fix_pending) {
		len += snprintf(payload + len, sizeof(payload) - len,
				",\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.0f,\"loc\":\"%s\","
				"\"fix_age\":%lld",
				pending_fix.lat, pending_fix.lon, (double)pending_fix.acc_m,
				pending_fix.method ? pending_fix.method : "gnss",
				(long long)((k_uptime_get() - fix_time_ms) / 1000));
	}
	len += snprintf(payload + len, sizeof(payload) - len, "}");

	int err = publish_raw(CONFIG_O2_MQTT_TOPIC, payload, false);

	if (err) {
		LOG_WRN("Publish failed: %d", err);
		return err;
	}

	LOG_INF("Sent: %s", payload);
	return 0;
}

#else /* iAQ-Core */

static int publish_iaq(const struct iaq_data *d)
{
	char payload[320];
	int len;

	if (d->status == IAQ_STATUS_OK) {
		len = snprintf(payload, sizeof(payload),
			       "{\"co2\":%u,\"tvoc\":%u,\"res\":%u,\"status\":\"ok\",\"alarm\":%s",
			       d->co2_ppm, d->tvoc_ppb, d->resistance_ohm,
			       alarm_active ? "true" : "false");
	} else {
		len = snprintf(payload, sizeof(payload), "{\"status\":\"%s\",\"alarm\":%s",
			       iaq_status_str(d->status), alarm_active ? "true" : "false");
	}

	if (env_ok) {
		float p_kpa, t_c;

		if (env_read(&p_kpa, &t_c) == 0) {
			len += snprintf(payload + len, sizeof(payload) - len,
					",\"p\":%.2f,\"t\":%.1f", (double)p_kpa, (double)t_c);
		}
	}
	if (fix_pending) {
		len += snprintf(payload + len, sizeof(payload) - len,
				",\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.0f,\"loc\":\"%s\","
				"\"fix_age\":%lld",
				pending_fix.lat, pending_fix.lon, (double)pending_fix.acc_m,
				pending_fix.method ? pending_fix.method : "gnss",
				(long long)((k_uptime_get() - fix_time_ms) / 1000));
	}
	len += snprintf(payload + len, sizeof(payload) - len, "}");

	int err = publish_raw(CONFIG_IAQ_MQTT_TOPIC, payload, false);

	if (err) {
		LOG_WRN("Publish failed: %d", err);
		return err;
	}

	LOG_INF("Sent: %s", payload);
	return 0;
}
#endif

/* ---------------- Alarm + LEDs ---------------- */

#if defined(CONFIG_APP_SENSOR_O2)
static bool update_alarm(const struct o2_reading *r)
{
	int level = (int)(r->o2_pct * 100.0f);
	bool was = alarm_active;

	if (!alarm_active && level < CONFIG_O2_ALARM_LEVEL_X100) {
		alarm_active = true;
		LOG_WRN("LOW O2 ALARM: %.2f %%", (double)r->o2_pct);
	} else if (alarm_active && level >= CONFIG_O2_RECOVER_LEVEL_X100) {
		alarm_active = false;
		LOG_INF("O2 recovered: %.2f %%", (double)r->o2_pct);
	}
	return was != alarm_active;
}
#else
/* Alarm if CO2-eq OR TVOC-eq is above its level; clears below the lower levels */
static bool update_alarm(const struct iaq_data *d)
{
	bool was = alarm_active;
	bool high = d->co2_ppm >= CONFIG_IAQ_CO2_ALARM_PPM ||
		    d->tvoc_ppb >= CONFIG_IAQ_TVOC_ALARM_PPB;
	bool clear = d->co2_ppm < CONFIG_IAQ_CO2_CLEAR_PPM &&
		     d->tvoc_ppb < CONFIG_IAQ_TVOC_CLEAR_PPB;

	if (!alarm_active && high) {
		alarm_active = true;
		LOG_WRN("AIR QUALITY ALARM: CO2-eq %u ppm, TVOC-eq %u ppb", d->co2_ppm,
			d->tvoc_ppb);
	} else if (alarm_active && clear) {
		alarm_active = false;
		LOG_INF("Air quality back to normal: CO2-eq %u ppm, TVOC-eq %u ppb",
			d->co2_ppm, d->tvoc_ppb);
	}
	return was != alarm_active;
}
#endif

static void update_leds(uint32_t tick, bool fault, bool sent)
{
	if (atomic_get(&button_down)) {
		return; /* keep the blue / purple "hold" indication */
	}

	bool blink = tick & 1;
	bool red = false, green = false, blue = false;

	if (alarm_active) {
		red = true;
	} else if (fault || !sensor_ok) {
		red = blink;
	} else if (warming_up) {
		blue = blink;
	} else if (!mqtt_connected) {
		red = blink;
		green = blink; /* yellow */
	} else if (sent) {
		green = true;
	}

	dk_set_led(LED_RED, red);
	dk_set_led(LED_GREEN, green);
	dk_set_led(LED_BLUE, blue);
}

#if defined(CONFIG_APP_SENSOR_O2)
static void cal_blink(void)
{
	static bool on;

	on = !on;
	dk_set_led(LED_BLUE, on);
}

static void flash_result(bool ok)
{
	uint8_t led = ok ? LED_GREEN : LED_RED;

	dk_set_led(LED_BLUE, 0);
	for (int i = 0; i < 3; i++) {
		dk_set_led(led, 1);
		k_msleep(300);
		dk_set_led(led, 0);
		k_msleep(300);
	}
}

static void do_calibration(const char *cmd, enum o2_cal_type type, float pct)
{
	char msg[160];

	if (!sensor_ok) {
		publish_cal_result(cmd, false, "Sensor not available");
		return;
	}

	dk_set_led(LED_RED, 0);
	dk_set_led(LED_GREEN, 0);
	publish_cal_result(cmd, true, "Calibration started - do not move the sensor");

	int err = o2_sensor_calibrate(type, pct, cal_blink, msg, sizeof(msg));

	flash_result(err == 0);
	publish_cal_result(cmd, err == 0, msg);
	publish_cal_info();
}

static void do_cal_reset(const char *cmd)
{
	o2_sensor_cal_reset();
	flash_result(true);
	publish_cal_result(cmd, true, "Calibration reset to datasheet values");
	publish_cal_info();
}

/* Minimal JSON helpers for the small command messages */
static bool json_str(const char *js, const char *key, char *out, size_t out_len)
{
	char pat[24];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(js, pat);
	if (!p || !(p = strchr(p + strlen(pat), ':')) || !(p = strchr(p, '"'))) {
		return false;
	}
	p++;
	size_t i = 0;

	while (*p && *p != '"' && i < out_len - 1) {
		out[i++] = *p++;
	}
	out[i] = '\0';
	return i > 0;
}

static bool json_num(const char *js, const char *key, double *out)
{
	char pat[24];
	const char *p;
	char *end;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(js, pat);
	if (!p || !(p = strchr(p + strlen(pat), ':'))) {
		return false;
	}
	*out = strtod(p + 1, &end);
	return end != p + 1;
}

static void handle_command(const char *js)
{
	char cmd[20];
	double pct, zero, span;

	if (!json_str(js, "cmd", cmd, sizeof(cmd))) {
		LOG_WRN("Bad command: %s", js);
		return;
	}
	LOG_INF("Command: %s", js);

	if (!strcmp(cmd, "cal_air")) {
		do_calibration(cmd, O2_CAL_AIR, 20.9f);
	} else if (!strcmp(cmd, "cal_span")) {
		if (!json_num(js, "pct", &pct)) {
			publish_cal_result(cmd, false, "Missing span gas concentration (pct)");
			return;
		}
		do_calibration(cmd, O2_CAL_SPAN, (float)pct);
	} else if (!strcmp(cmd, "cal_zero")) {
		do_calibration(cmd, O2_CAL_ZERO, 0.0f);
	} else if (!strcmp(cmd, "cal_reset")) {
		do_cal_reset(cmd);
	} else if (!strcmp(cmd, "cal_set")) {
		char msg[160];

		if (!json_num(js, "zero_uv", &zero) || !json_num(js, "span_uv", &span) ||
		    !json_num(js, "pct", &pct)) {
			publish_cal_result(cmd, false, "cal_set needs zero_uv, span_uv and pct");
			return;
		}
		int err = o2_sensor_cal_set((int32_t)zero, (int32_t)span, (float)pct, msg,
					    sizeof(msg));

		publish_cal_result(cmd, err == 0, msg);
		publish_cal_info();
	} else if (!strcmp(cmd, "cal_info")) {
		publish_cal_info();
	} else {
		publish_cal_result(cmd, false, "Unknown command");
	}
}
#endif

/* ---------------- Main ---------------- */

/* Connect / service the network, shared by both sensor builds */
static void network_step(int64_t now, int64_t *next_connect, int64_t *next_publish)
{
	struct gnss_fix fix;

	/* New GNSS fix? keep it until it has been sent */
	double mlat, mlon;
	bool manual = manual_loc_get(&mlat, &mlon);

	/* Manual location typed on the serial port ("loc set" / "loc clear") */
	if (manual_loc_changed()) {
		if (manual) {
			pending_fix = (struct gnss_fix){ .lat = mlat, .lon = mlon,
							 .acc_m = 0.0f, .method = "manual" };
			fix_pending = true;
			fix_time_ms = now;
		} else {
			fix_pending = false; /* wait for a real position */
		}
		*next_publish = 0;
	}

	if (gnss_take_new_fix(&fix) && !manual) {
		pending_fix = fix;
		fix_pending = true;
		fix_time_ms = now;
		*next_publish = 0;  /* send the new position right away */
	}

	static bool gnss_started;

	if (!gnss_started && atomic_get(&lte_ready) &&
	    !IS_ENABLED(CONFIG_APP_FIXED_LOCATION_ALWAYS)) {
		gnss_started = true;
		if (gnss_start()) {
			LOG_WRN("GNSS not started - continuing without location");
		}
	}

	if (gnss_started) {
		gnss_service();
	}

	if (!mqtt_open && atomic_get(&lte_ready) && now >= *next_connect) {
		if (mqtt_start() != 0) {
			*next_connect = now + RECONNECT_DELAY_MS;
		}
	}
}

int main(void)
{
	int err;
	uint32_t tick = 0;
	int64_t next_publish = 0;
	int64_t next_connect = 0;

	LOG_INF("Thingy:91 X air monitor starting (%s)",
		IS_ENABLED(CONFIG_APP_SENSOR_O2) ? "O2 sensor" : "iAQ-Core");

	dk_leds_init();
	dk_buttons_init(button_handler);

	env_ok = (env_init() == 0);
#if defined(CONFIG_APP_SENSOR_O2)
	sensor_ok = (o2_sensor_init() == 0);
#else
	sensor_ok = (iaq_init() == 0);
	warming_up = sensor_ok;
#endif

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed: %d", err);
	}

	read_imei_client_id();

	if (provision_ca_cert()) {
		LOG_ERR("CA certificate provisioning failed - TLS will not work");
	}

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("LTE connect failed to start: %d", err);
	} else {
		LOG_INF("Connecting to LTE (can take 1-2 minutes)...");
	}

	/* GNSS is started once LTE has registered (see network_step):
	 * GNSS and LTE share the radio, and searching for both at the same
	 * time can delay the first LTE connection by minutes.
	 */

	manual_loc_init();

	while (true) {
		bool sent = false;
		bool fault = false;
		int64_t now;

#if defined(CONFIG_APP_SENSOR_O2)
		struct o2_reading r = { 0 };
		bool have_reading = false;

		/* 1. Calibration from the button or an MQTT command */
		atomic_val_t req = atomic_set(&button_req, REQ_NONE);

		if (req == REQ_CAL_AIR) {
			do_calibration("button_air", O2_CAL_AIR, 20.9f);
			next_publish = 0;
		} else if (req == REQ_CAL_RESET) {
			do_cal_reset("button_reset");
			next_publish = 0;
		}
		if (atomic_get(&cmd_pending)) {
			handle_command(cmd_buf);
			atomic_set(&cmd_pending, 0);
			next_publish = 0;
		}

		/* Pressure / temperature for compensation */
		if (env_ok && (tick % MAX(1, (CONFIG_O2_ENV_INTERVAL_S * 1000) /
					     CONFIG_O2_SAMPLE_INTERVAL_MS)) == 0) {
			float p_kpa = 0.0f, t_c = 0.0f;
			bool valid = env_read(&p_kpa, &t_c) == 0;

			o2_sensor_set_env(valid, p_kpa, t_c);
		}

		/* 2. Read the sensor (retry init if it failed at boot) */
		if (!sensor_ok && (tick % 10) == 0) {
			sensor_ok = (o2_sensor_init() == 0);
		}
		if (sensor_ok && o2_sensor_sample() == 0) {
			have_reading = o2_sensor_get(&r);
		}
		fault = have_reading && r.fault;

		/* 3. Local alarm (works even with no network) */
		bool alarm_changed = have_reading && update_alarm(&r);

		/* 4. Network */
		now = k_uptime_get();
		network_step(now, &next_connect, &next_publish);

		/* 5. Publish on schedule, or immediately on alarm change / new fix */
		if (have_reading && mqtt_connected && (now >= next_publish || alarm_changed)) {
			if (publish_reading(&r) == 0) {
				sent = true;
				next_publish = now + CONFIG_O2_PUBLISH_INTERVAL_S * MSEC_PER_SEC;
			}
		}
#else /* iAQ-Core */
		static struct iaq_data d;
		static bool have_data;
		static bool iaq_fault;
		bool alarm_changed = false;

		if (atomic_set(&button_req, REQ_NONE) == REQ_SEND_NOW) {
			next_publish = 0;
		}

		/* 1. Read the sensor every CONFIG_IAQ_READ_INTERVAL_S */
		if (!sensor_ok && (tick % 10) == 0) {
			sensor_ok = (iaq_init() == 0);
		}
		if (!sensor_ok) {
			/* Still report in, so the connection can be tested without the sensor */
			d = (struct iaq_data){ .status = IAQ_STATUS_NO_SENSOR };
			have_data = true;
			warming_up = false;
		}
		if (sensor_ok && (tick % CONFIG_IAQ_READ_INTERVAL_S) == 0) {
			if (iaq_read(&d) == 0) {
				have_data = true;
				warming_up = (d.status == IAQ_STATUS_RUNIN);
				iaq_fault = (d.status == IAQ_STATUS_ERROR);

				/* 2. Local alarm on valid data only */
				if (d.status == IAQ_STATUS_OK) {
					alarm_changed = update_alarm(&d);
				}
			} else {
				iaq_fault = true;
			}
		}
		fault = iaq_fault;

		/* 3. Network */
		now = k_uptime_get();
		network_step(now, &next_connect, &next_publish);

		/* 4. Publish on schedule, or immediately on alarm change / new fix / button */
		if (have_data && mqtt_connected && (now >= next_publish || alarm_changed)) {
			if (publish_iaq(&d) == 0) {
				sent = true;
				next_publish = now + CONFIG_IAQ_PUBLISH_INTERVAL_S * MSEC_PER_SEC;
			}
		}
#endif

		update_leds(tick, fault, sent);
		tick++;

		/* Wait ~1 s while servicing MQTT */
		mqtt_service(MSEC_PER_SEC);
	}

	return 0;
}
