/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Measurements Specialties common sensor driver
 *
 * Copyright (c) 2015 Measurement-Specialties
 */

#ifndef _MS_SENSORS_I2C_H
#define _MS_SENSORS_I2C_H

#include <linux/i2c.h>
#include <linux/mutex.h>

#define MS_SENSORS_TP_PROM_WORDS_NB		8

/**
 * struct ms_ht_dev - Humidity/Temperature sensor device structure
 * @client:	i2c client
 * @lock:	lock protecting the i2c conversion
 * @res_index:	index to selected sensor resolution
 */
struct ms_ht_dev {
	struct i2c_client *client;
	struct mutex lock;
	u8 res_index;
};

/**
 * struct ms_hw_data - Temperature/Pressure sensor hardware data
 * @prom_len:		number of words in the PROM
 * @max_res_index:	maximum sensor resolution index
 */
struct ms_tp_hw_data {
	u8 prom_len;
	u8 max_res_index;
};

/**
 * struct ms_tp_comp_consts - Temperature compensation constants
 * @press_scale: pressure scale
 * @high_t2_multiplier: multiplier for t2 in high temperature state
 * @high_t2_shift: bit shift for t2 in high temperature state
 * @high_off2_multiplier: multiplier for off2 in high temperature state
 * @high_off2_shift: bit shift for off2 in high temperature state
 * @low_t2_multiplier: multiplier for t2 in low temperature state
 * @low_t2_shift: bit shift for t2 in low temperature state
 * @low_off2_multiplier: multiplier for off2 in low temperature state
 * @low_off2_shift: bit shift for off2 in low temperature state
 * @low_sens2_multiplier: multiplier for sens2 in low temperature state
 * @low_sens2_shift: bit shift for sens2 in low temperature state
 * @vlow_off2_multiplier: multiplier for value added to off2 in very low temperature state
 * @vlow_sens2_multiplier: multiplier for value added to sens2 in very low temperature state
 * @has_vhigh_temp: has very high temperature compensation logic
 * @off_t1_shift: temperature offset t1 bit shift
 * @off_shift: temperature offset shift
 * @sens_t1_shift: temperature sensitivity t1 shift
 * @sens_shift: temperature sensitivity shift
 * @press_sens_shift: pressure sensitivity shift
 * @press_shift: pressure shift
 */
struct ms_tp_comp_consts {
	u32 press_scale;
	u8 high_t2_multiplier;
	u8 high_t2_shift;
	u8 high_off2_multiplier;
	u8 high_off2_shift;
	u8 low_t2_multiplier;
	u8 low_t2_shift;
	u8 low_off2_multiplier;
	u8 low_off2_shift;
	u8 low_sens2_multiplier;
	u8 low_sens2_shift;
	u8 vlow_off2_multiplier;
	u8 vlow_sens2_multiplier;
	bool has_vhigh_temp;
	u8 off_t1_shift;
	u8 off_shift;
	u8 sens_t1_shift;
	u8 sens_shift;
	u8 press_sens_shift;
	u8 press_shift;
};

/**
 * struct ms_tp_data - Temperature/Pressure sensor data
 * @name: Device name
 * @hw: Sensor hardware data
 * @comp_consts: Temperature compensation constants
 */
struct ms_tp_data {
	const char *name;
	const struct ms_tp_hw_data *hw;
	const struct ms_tp_comp_consts *comp_consts;
};

/**
 * struct ms_tp_dev - Temperature/Pressure sensor device structure
 * @client:	i2c client
 * @lock:	lock protecting the i2c conversion
 * @prom:	array of PROM coefficients used for conversion. Added element
 *              for CRC computation
 * @res_index:	index to selected sensor resolution
 * @data:	Temperature/Pressure sensor data
 */
struct ms_tp_dev {
	struct i2c_client *client;
	struct mutex lock;
	const struct ms_tp_data *data;
	u16 prom[MS_SENSORS_TP_PROM_WORDS_NB];
	u8 res_index;
};

int ms_sensors_reset(void *cli, u8 cmd, unsigned int delay);
int ms_sensors_read_prom_word(void *cli, int cmd, u16 *word);
int ms_sensors_convert_and_read(void *cli, u8 conv, u8 rd,
				unsigned int delay, u32 *adc);
int ms_sensors_read_serial(struct i2c_client *client, u64 *sn);
ssize_t ms_sensors_show_serial(struct ms_ht_dev *dev_data, char *buf);
ssize_t ms_sensors_write_resolution(struct ms_ht_dev *dev_data, u8 i);
ssize_t ms_sensors_show_battery_low(struct ms_ht_dev *dev_data, char *buf);
ssize_t ms_sensors_show_heater(struct ms_ht_dev *dev_data, char *buf);
ssize_t ms_sensors_write_heater(struct ms_ht_dev *dev_data,
				const char *buf, size_t len);
int ms_sensors_ht_read_temperature(struct ms_ht_dev *dev_data,
				   s32 *temperature);
int ms_sensors_ht_read_humidity(struct ms_ht_dev *dev_data,
				u32 *humidity);
int ms_sensors_tp_read_prom(struct ms_tp_dev *dev_data);
int ms_sensors_read_temp_and_pressure(struct ms_tp_dev *dev_data,
				      int *temperature,
				      unsigned int *pressure);

#endif /* _MS_SENSORS_I2C_H */
