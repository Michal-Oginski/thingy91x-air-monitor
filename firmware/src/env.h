#ifndef ENV_H_
#define ENV_H_

/* Built-in BME688 on the Thingy:91 X. */
struct env_reading {
	float pressure_kpa;
	float temperature_c;
	float humidity_pct;
	float gas_resistance_ohm;
};

int env_init(void);
int env_read(struct env_reading *reading);

#endif /* ENV_H_ */
