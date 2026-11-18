#ifndef IAQ_CORE_H_
#define IAQ_CORE_H_

#include <stdint.h>

/* Status byte values from the iAQ-Core datasheet */
#define IAQ_STATUS_OK    0x00
#define IAQ_STATUS_BUSY  0x01
#define IAQ_STATUS_RUNIN 0x10 /* warm-up, first valid reading after ~5 min */
#define IAQ_STATUS_ERROR 0x80
#define IAQ_STATUS_NO_SENSOR 0xFE /* not from the sensor: not detected on I2C */

struct iaq_data {
	uint16_t co2_ppm;        /* CO2 equivalent prediction */
	uint16_t tvoc_ppb;       /* TVOC equivalent prediction */
	uint32_t resistance_ohm; /* sensor resistance */
	uint8_t status;
};

int iaq_init(void);

/* Read all 9 bytes. Returns 0 if the bus transfer worked (check d->status). */
int iaq_read(struct iaq_data *d);

const char *iaq_status_str(uint8_t status);

#endif /* IAQ_CORE_H_ */
