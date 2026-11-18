#ifndef GNSS_H_
#define GNSS_H_

#include <stdbool.h>

/* A position from GNSS, Wi-Fi or cellular (Location library) */
struct gnss_fix {
	double lat;
	double lon;
	float acc_m;
	const char *method; /* "gnss", "wifi" or "cell" */
};

/* Connect to nRF Cloud (for Wi-Fi/cell lookups and A-GNSS) and start
 * periodic location requests. Call after LTE is connected. Blocks for a
 * few seconds while connecting to nRF Cloud.
 */
int gnss_start(void);

/* Returns true (once) when a new position has arrived since the last call. */
bool gnss_take_new_fix(struct gnss_fix *out);

/* Call from the main loop: reconnects to nRF Cloud after lookup errors. */
void gnss_service(void);

#endif /* GNSS_H_ */
