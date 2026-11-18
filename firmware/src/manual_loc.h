#ifndef MANUAL_LOC_H_
#define MANUAL_LOC_H_

#include <stdbool.h>

/* Load the manual location saved in flash (set with the "loc" serial command). */
void manual_loc_init(void);

/* True while a manual location is set (it then overrides GNSS/Wi-Fi/cell). */
bool manual_loc_get(double *lat, double *lon);

/* Returns true (once) after "loc set" or "loc clear" was typed. */
bool manual_loc_changed(void);

#endif /* MANUAL_LOC_H_ */
