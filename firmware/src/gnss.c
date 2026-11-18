/*
 * Location using the nRF Connect SDK Location library:
 *   Wi-Fi (nRF7002 scan)  -> position looked up by the nRF Cloud location service
 *   GNSS                  -> on the device, sped up with A-GNSS from nRF Cloud
 *   Cellular              -> nearby cell towers, looked up by nRF Cloud
 *
 * The library tries the methods in order and stops at the first success.
 * Only the location lookups go to nRF Cloud (CoAP); sensor data still goes
 * to your own MQTT server.
 *
 * The device must be registered in your nRF Cloud account (Quick Start does
 * this for the Thingy:91 X).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <modem/location.h>
#if defined(CONFIG_NRF_CLOUD_COAP)
#include <net/nrf_cloud_coap.h>
#endif

#include "gnss.h"

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

static struct k_spinlock lock;
static struct gnss_fix latest;
static bool have_new;
static atomic_t cloud_reconnect;
static bool cloud_ok;

static const char *method_name(enum location_method m)
{
	switch (m) {
	case LOCATION_METHOD_GNSS:
		return "gnss";
#if defined(CONFIG_LOCATION_METHOD_WIFI)
	case LOCATION_METHOD_WIFI:
		return "wifi";
#endif
#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
	case LOCATION_METHOD_CELLULAR:
		return "cell";
#endif
	default:
		return "other";
	}
}

static void location_handler(const struct location_event_data *ev)
{
	switch (ev->id) {
	case LOCATION_EVT_LOCATION: {
		k_spinlock_key_t key = k_spin_lock(&lock);

		latest.lat = ev->location.latitude;
		latest.lon = ev->location.longitude;
		latest.acc_m = ev->location.accuracy;
		latest.method = method_name(ev->method);
		have_new = true;
		k_spin_unlock(&lock, key);

		LOG_INF("Location (%s): %.6f, %.6f (+/- %.0f m)", method_name(ev->method),
			ev->location.latitude, ev->location.longitude,
			(double)ev->location.accuracy);
		break;
	}
	case LOCATION_EVT_TIMEOUT:
		LOG_WRN("Location: no position from any method this time");
		break;
	case LOCATION_EVT_ERROR:
		LOG_WRN("Location: request failed (nRF Cloud lookup?) - will reconnect");
		atomic_set(&cloud_reconnect, 1);
		break;
	case LOCATION_EVT_FALLBACK:
		LOG_INF("Location: %s failed, trying the next method",
			method_name(ev->method));
		break;
	default:
		break;
	}
}

static int cloud_connect(void)
{
#if defined(CONFIG_NRF_CLOUD_COAP)
	int err = nrf_cloud_coap_connect(NULL);

	if (err) {
		LOG_WRN("nRF Cloud connect failed (%d) - Wi-Fi/cell location and A-GNSS "
			"unavailable. Is the device registered in your nRF Cloud account?",
			err);
		return err;
	}
	LOG_INF("Connected to nRF Cloud (location service)");
#endif
	return 0;
}

int gnss_start(void)
{
	int err;

#if defined(CONFIG_NRF_CLOUD_COAP)
	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nRF Cloud CoAP init failed: %d", err);
	} else {
		cloud_ok = (cloud_connect() == 0);
	}
#endif

	err = location_init(location_handler);
	if (err) {
		LOG_ERR("location_init failed: %d", err);
		return err;
	}

	struct location_config cfg;
	enum location_method methods[] = {
#if defined(CONFIG_LOCATION_METHOD_WIFI) && defined(CONFIG_APP_LOCATION_WIFI_FIRST)
		LOCATION_METHOD_WIFI,
		LOCATION_METHOD_GNSS,
#else
		LOCATION_METHOD_GNSS,
#if defined(CONFIG_LOCATION_METHOD_WIFI)
		LOCATION_METHOD_WIFI,
#endif
#endif
#if defined(CONFIG_LOCATION_METHOD_CELLULAR)
		LOCATION_METHOD_CELLULAR,
#endif
	};

	location_config_defaults_set(&cfg, ARRAY_SIZE(methods), methods);

	cfg.interval = CONFIG_O2_GNSS_INTERVAL_S; /* 0 = single request */
	cfg.timeout = (CONFIG_O2_GNSS_TIMEOUT_S + 120) * MSEC_PER_SEC;

	for (int i = 0; i < cfg.methods_count; i++) {
		if (cfg.methods[i].method == LOCATION_METHOD_GNSS) {
			cfg.methods[i].gnss.timeout = CONFIG_O2_GNSS_TIMEOUT_S * MSEC_PER_SEC;
			cfg.methods[i].gnss.accuracy = LOCATION_ACCURACY_NORMAL;
			/* Without PSM from the network, LTE leaves GNSS almost no radio
			 * time; let GNSS take priority briefly when needed.
			 */
			cfg.methods[i].gnss.priority_mode = true;
		}
	}

	err = location_request(&cfg);
	if (err) {
		LOG_ERR("location_request failed: %d", err);
		return err;
	}

	LOG_INF("Location started: %s, every %d s",
		IS_ENABLED(CONFIG_LOCATION_METHOD_WIFI)
			? (IS_ENABLED(CONFIG_APP_LOCATION_WIFI_FIRST) ? "Wi-Fi -> GNSS -> cell"
								      : "GNSS -> Wi-Fi -> cell")
			: "GNSS -> cell",
		CONFIG_O2_GNSS_INTERVAL_S);
	return 0;
}

void gnss_service(void)
{
#if defined(CONFIG_NRF_CLOUD_COAP)
	static int64_t next_try;

	if ((atomic_get(&cloud_reconnect) || !cloud_ok) && k_uptime_get() >= next_try) {
		atomic_set(&cloud_reconnect, 0);
		next_try = k_uptime_get() + 60 * MSEC_PER_SEC;
		nrf_cloud_coap_disconnect();
		cloud_ok = (cloud_connect() == 0);
	}
#endif
}

bool gnss_take_new_fix(struct gnss_fix *out)
{
	bool ret;
	k_spinlock_key_t key = k_spin_lock(&lock);

	ret = have_new;
	if (ret) {
		*out = latest;
		have_new = false;
	}
	k_spin_unlock(&lock, key);
	return ret;
}
