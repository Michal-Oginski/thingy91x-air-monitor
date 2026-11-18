/*
 * Manual location over the serial terminal (115200 baud), saved in flash:
 *
 *   loc set -26.107600 28.056700   use this position (overrides GNSS/Wi-Fi/cell)
 *   loc clear                      back to automatic location
 *   loc show                       print the current setting
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "manual_loc.h"

LOG_MODULE_REGISTER(manual_loc, LOG_LEVEL_INF);

struct loc_store {
	double lat;
	double lon;
};

static struct k_spinlock lock;
static struct loc_store loc;
static bool active;
static atomic_t changed;

static int loc_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "pos", &next) && !next) {
		struct loc_store tmp;

		if (len == sizeof(tmp) && read_cb(cb_arg, &tmp, sizeof(tmp)) == sizeof(tmp)) {
			loc = tmp;
			active = true;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(manual_loc, "loc", NULL, loc_settings_set, NULL, NULL);

void manual_loc_init(void)
{
	if (settings_subsys_init() == 0) {
		settings_load_subtree("loc");
	}

	/* Fallback: location from prj.conf (CONFIG_APP_FIXED_LAT / _LON) */
	if (!active && strlen(CONFIG_APP_FIXED_LAT) > 0 && strlen(CONFIG_APP_FIXED_LON) > 0) {
		loc.lat = strtod(CONFIG_APP_FIXED_LAT, NULL);
		loc.lon = strtod(CONFIG_APP_FIXED_LON, NULL);
		active = true;
	}

	if (active) {
		LOG_INF("Manual location: %.6f, %.6f (type 'loc clear' for automatic)", loc.lat,
			loc.lon);
		atomic_set(&changed, 1);
	} else {
		LOG_INF("No manual location. Set one with: loc set <lat> <lon>");
	}
}

bool manual_loc_get(double *lat, double *lon)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool ret = active;

	*lat = loc.lat;
	*lon = loc.lon;
	k_spin_unlock(&lock, key);
	return ret;
}

bool manual_loc_changed(void)
{
	return atomic_cas(&changed, 1, 0);
}

/* ---------------- Shell commands ---------------- */

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	char *end1, *end2;
	double lat = strtod(argv[1], &end1);
	double lon = strtod(argv[2], &end2);

	if (*end1 || *end2 || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
		shell_error(sh, "Use decimal degrees, e.g.: loc set -26.107600 28.056700");
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	loc.lat = lat;
	loc.lon = lon;
	active = true;
	k_spin_unlock(&lock, key);

	struct loc_store save = { .lat = lat, .lon = lon };
	int err = settings_save_one("loc/pos", &save, sizeof(save));

	atomic_set(&changed, 1);
	shell_print(sh, "Manual location set: %.6f, %.6f%s - sending now", lat, lon,
		    err ? " (NOT saved to flash)" : " (saved)");
	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	active = false;
	k_spin_unlock(&lock, key);

	settings_delete("loc/pos");
	atomic_set(&changed, 1);
	shell_print(sh, "Manual location cleared - using GNSS / Wi-Fi / cell again");
	return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	double lat, lon;

	if (manual_loc_get(&lat, &lon)) {
		shell_print(sh, "Manual location: %.6f, %.6f", lat, lon);
	} else {
		shell_print(sh, "No manual location (automatic GNSS / Wi-Fi / cell)");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(loc_cmds,
	SHELL_CMD_ARG(set, NULL, "Set location: loc set <lat> <lon>", cmd_set, 3, 0),
	SHELL_CMD(clear, NULL, "Back to automatic location", cmd_clear),
	SHELL_CMD(show, NULL, "Show the current location setting", cmd_show),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(loc, &loc_cmds, "Manual location for the dashboard map", NULL);
