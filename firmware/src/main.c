#include <stdio.h>
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

#include "gnss.h"
#include "manual_loc.h"
#include "env.h"

LOG_MODULE_REGISTER(thingy91x_main, LOG_LEVEL_INF);

#if !defined(NET_AF_INET)
#define NET_AF_INET AF_INET
#define NET_SOCK_STREAM SOCK_STREAM
#define net_sockaddr_in sockaddr_in
#define net_sockaddr_storage sockaddr_storage
#define net_htons htons
#endif
#if !defined(ZSOCK_TLS_PEER_VERIFY_REQUIRED)
#define ZSOCK_TLS_PEER_VERIFY_REQUIRED TLS_PEER_VERIFY_REQUIRED
#define ZSOCK_TLS_SESSION_CACHE_DISABLED TLS_SESSION_CACHE_DISABLED
#endif

#define LED_RED DK_LED1
#define LED_GREEN DK_LED2
#define LED_BLUE DK_LED3
#define RECONNECT_DELAY_MS (30 * MSEC_PER_SEC)

/* Mikrus MQTT broker CA, embedded at build time */
static const char ca_cert[] = {
#include "ca_cert.pem.inc"
	0x00
};

static atomic_t lte_ready;
static atomic_t button_send;
static struct mqtt_client client;
static struct net_sockaddr_storage broker;
static struct zsock_pollfd fds;
static bool mqtt_open;
static bool mqtt_connected;
static uint16_t msg_id;
static uint8_t rx_buf[256];
static uint8_t tx_buf[768];
static char client_id[48] = "thingy91x";
static bool env_ok;
static struct gnss_fix pending_fix;
static bool fix_pending;
static int64_t fix_time_ms;

static void button_handler(uint32_t state, uint32_t changed)
{
	if ((changed & DK_BTN1_MSK) && (state & DK_BTN1_MSK)) {
		atomic_set(&button_send, 1);
	}
}

static void lte_handler(const struct lte_lc_evt *evt)
{
	if (evt->type == LTE_LC_EVT_NW_REG_STATUS) {
		bool registered = evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
				  evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;

		if (registered != (bool)atomic_get(&lte_ready)) {
			LOG_INF("LTE %s", registered ? "connected" : "disconnected");
		}
		atomic_set(&lte_ready, registered);
	}
}

static int provision_ca_cert(void)
{
	bool exists = false;
	int err = modem_key_mgmt_exists(CONFIG_APP_SEC_TAG,
					MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);

	if (err) {
		LOG_ERR("modem_key_mgmt_exists: %d", err);
		return err;
	}

	if (exists &&
	    modem_key_mgmt_cmp(CONFIG_APP_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
			       ca_cert, strlen(ca_cert)) == 0) {
		return 0;
	}

	if (exists) {
		modem_key_mgmt_delete(CONFIG_APP_SEC_TAG,
				      MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
	}

	LOG_INF("Writing CA certificate to sec tag %d", CONFIG_APP_SEC_TAG);
	return modem_key_mgmt_write(CONFIG_APP_SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    ca_cert, strlen(ca_cert));
}

static void read_imei_client_id(void)
{
	char imei[32] = { 0 };

	if (nrf_modem_at_cmd(imei, sizeof(imei), "AT+CGSN") == 0) {
		imei[15] = '\0';
		snprintf(client_id, sizeof(client_id), "thingy91x-%s", imei);
	}
	LOG_INF("MQTT client ID: %s", client_id);
}

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			mqtt_connected = true;
			LOG_INF("MQTT connected to %s", CONFIG_APP_MQTT_HOST);
		} else {
			LOG_ERR("MQTT connection refused (%d)", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected (%d)", evt->result);
		mqtt_connected = false;
		mqtt_open = false;
		break;
	case MQTT_EVT_PUBACK:
	case MQTT_EVT_PINGRESP:
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
	int err = zsock_getaddrinfo(CONFIG_APP_MQTT_HOST, NULL, &hints, &res);

	if (err) {
		LOG_ERR("DNS lookup of %s failed: %d", CONFIG_APP_MQTT_HOST, err);
		return -EIO;
	}

	struct net_sockaddr_in *b4 = (struct net_sockaddr_in *)&broker;
	memcpy(b4, res->ai_addr, sizeof(*b4));
	b4->sin_family = NET_AF_INET;
	b4->sin_port = net_htons(CONFIG_APP_MQTT_PORT);
	zsock_freeaddrinfo(res);
	return 0;
}

static int mqtt_start(void)
{
	static sec_tag_t sec_tags[] = { CONFIG_APP_SEC_TAG };
	static struct mqtt_utf8 user = {
		.utf8 = (uint8_t *)CONFIG_APP_MQTT_USER,
		.size = sizeof(CONFIG_APP_MQTT_USER) - 1,
	};
	static struct mqtt_utf8 pass = {
		.utf8 = (uint8_t *)CONFIG_APP_MQTT_PASSWORD,
		.size = sizeof(CONFIG_APP_MQTT_PASSWORD) - 1,
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
	client.keepalive = CONFIG_APP_MQTT_KEEPALIVE_S;
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
	tls->hostname = CONFIG_APP_MQTT_HOST;
	tls->session_cache = ZSOCK_TLS_SESSION_CACHE_DISABLED;

	mqtt_connected = false;
	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect failed: %d", err);
		return err;
	}

	mqtt_open = true;
	fds.fd = client.transport.tls.sock;
	fds.events = ZSOCK_POLLIN;

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
			mqtt_abort(&client);
			mqtt_open = false;
			mqtt_connected = false;
		}
	}

	if (mqtt_open) {
		mqtt_live(&client);
	}
}

static int publish_environment(const struct env_reading *reading)
{
	char payload[320];
	int len = snprintf(payload, sizeof(payload),
			   "{\"status\":\"ok\",\"temperature\":%.2f,\"pressure\":%.2f,"
			   "\"humidity\":%.2f,\"gas_resistance\":%.0f",
			   (double)reading->temperature_c, (double)reading->pressure_kpa,
			   (double)reading->humidity_pct,
			   (double)reading->gas_resistance_ohm);

	if (fix_pending) {
		len += snprintf(payload + len, sizeof(payload) - len,
				",\"lat\":%.6f,\"lon\":%.6f,\"acc\":%.0f,\"loc\":\"%s\","
				"\"fix_age\":%lld",
				pending_fix.lat, pending_fix.lon,
				(double)pending_fix.acc_m,
				pending_fix.method ? pending_fix.method : "gnss",
				(long long)((k_uptime_get() - fix_time_ms) / 1000));
	}
	len += snprintf(payload + len, sizeof(payload) - len, "}");

	struct mqtt_publish_param publish = {
		.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.message.topic.topic.utf8 = (uint8_t *)CONFIG_APP_MQTT_TOPIC,
		.message.topic.topic.size = sizeof(CONFIG_APP_MQTT_TOPIC) - 1,
		.message.payload.data = (uint8_t *)payload,
		.message.payload.len = strlen(payload),
		.message_id = ++msg_id,
		.retain_flag = 0,
	};

	if (!mqtt_connected) {
		return -ENOTCONN;
	}

	int err = mqtt_publish(&client, &publish);
	if (err) {
		LOG_WRN("Publish failed: %d", err);
	} else {
		LOG_INF("Sent: %s", payload);
	}
	return err;
}

static void network_step(int64_t now, int64_t *next_connect, int64_t *next_publish)
{
	struct gnss_fix fix;
	double manual_lat;
	double manual_lon;
	bool manual = manual_loc_get(&manual_lat, &manual_lon);

	if (manual_loc_changed()) {
		if (manual) {
			pending_fix = (struct gnss_fix){
				.lat = manual_lat,
				.lon = manual_lon,
				.acc_m = 0.0f,
				.method = "manual",
			};
			fix_pending = true;
			fix_time_ms = now;
		} else {
			fix_pending = false;
		}
		*next_publish = 0;
	}

	if (gnss_take_new_fix(&fix) && !manual) {
		pending_fix = fix;
		fix_pending = true;
		fix_time_ms = now;
		*next_publish = 0;
	}

	static bool gnss_started;
	if (!gnss_started && atomic_get(&lte_ready) &&
	    !IS_ENABLED(CONFIG_APP_FIXED_LOCATION_ALWAYS)) {
		gnss_started = true;
		if (gnss_start()) {
			LOG_WRN("GNSS/location service not started");
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

static void update_leds(uint32_t tick, bool sent)
{
	bool blink = tick & 1;
	bool red = false;
	bool green = false;
	bool blue = false;

	if (!env_ok) {
		red = blink;
	} else if (!mqtt_connected) {
		red = blink;
		green = blink;
	} else if (sent) {
		green = true;
	}

	dk_set_led(LED_RED, red);
	dk_set_led(LED_GREEN, green);
	dk_set_led(LED_BLUE, blue);
}

int main(void)
{
	int err;
	uint32_t tick = 0;
	int64_t next_publish = 0;
	int64_t next_connect = 0;

	LOG_INF("Thingy:91 X environment monitor starting");

	dk_leds_init();
	dk_buttons_init(button_handler);
	env_ok = env_init() == 0;

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

	manual_loc_init();

	while (true) {
		bool sent = false;
		int64_t now = k_uptime_get();

		if (atomic_set(&button_send, 0)) {
			next_publish = 0;
		}

		network_step(now, &next_connect, &next_publish);

		static int64_t last_environment_sample = -1000;
		static struct env_reading environment;
		if (env_ok &&
		    now - last_environment_sample >=
		    CONFIG_APP_ENV_SAMPLE_INTERVAL_S * MSEC_PER_SEC) {
			last_environment_sample = now;
			env_ok = env_read(&environment) == 0;
		}

		if (env_ok && mqtt_connected &&
		    (now >= next_publish ||
		     atomic_get(&button_send))) {
			if (publish_environment(&environment) == 0) {
				sent = true;
				next_publish = now +
					CONFIG_APP_ENV_PUBLISH_INTERVAL_S * MSEC_PER_SEC;
			}
		}

		update_leds(tick++, sent);
		mqtt_service(MSEC_PER_SEC);
	}

	return 0;
}
