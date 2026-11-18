#ifndef ENV_H_
#define ENV_H_

/* Built-in BME688 on the Thingy:91 X: pressure and temperature. */
int env_init(void);
int env_read(float *pressure_kpa, float *temp_c);

#endif /* ENV_H_ */
