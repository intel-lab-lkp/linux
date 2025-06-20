// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp3_selftest.c
 * @date		2024-12-04
 * @version		v2.1.0
 *
 */

#include "bmp3_selftest.h"

/***************** Static function declarations ******************************/
/*!
 * @brief       Function to analyze the sensor data
 *
 * @param[in]   data   Structure instance of bmp3_data
 * (compensated temp & press values)
 *
 * @return Result of API execution status
 * @retval 0 -> Success
 * @retval >0 -> Warning
 * @retval <0 -> Fail
 */
static s8 analyze_sensor_data(const struct bmp3_data *data);
/*!
 * @brief       Function to calculate the CRC of the trimming parameters.
 *
 * @param[in]   seed   CRC of each register
 * @param[in]   data   register data.
 *
 * @return      calculated CRC
 */
static s8 cal_crc(u8 seed, u8 data);
/*!
 * @brief Function to validate the trimming parameters
 *
 * @param [in] dev Structure instance of bmp3_dev structure.
 *
 * @return Result of API execution status
 * @retval 0 -> Success
 * @retval >0 -> Warning
 * @retval <0 -> Fail
 */
static s8 validate_trimming_param(struct bmp3_dev *dev);
/****************** Global Function Definitions *******************************/

/*!
 * @brief       Self-test API for the BMP38X
 */
s8 bmp3_selftest_check(struct bmp3_settings *settings, struct bmp3_dev *dev)
{
	s8 rslt;
	/* Variable used to select the sensor component */
	u8 sensor_comp;
	/* Variable used to store the compensated data */
	struct bmp3_data data = { 0 };
	/* Used to select the settings user needs to change */
	u16 settings_sel;

	/* Reset the sensor */
	rslt = bmp3_soft_reset(dev);
	if (rslt == BMP3_SENSOR_OK) {
		rslt = bmp3_init(dev);
		if (rslt == BMP3_E_COMM_FAIL || rslt == BMP3_E_DEV_NOT_FOUND)
			rslt = BMP3_COMMUNICATION_ERROR_OR_WRONG_DEVICE;

		if (rslt == BMP3_SENSOR_OK)
			rslt = validate_trimming_param(dev);

		if (rslt == BMP3_SENSOR_OK) {
			/* Select the pressure and temperature sensor to be enabled */
			settings->press_en = BMP3_ENABLE;
			settings->temp_en = BMP3_ENABLE;
			/*
			 * Select the output data rate and over sampling settings
			 * for pressure and temperature
			 */
			settings->odr_filter.press_os = BMP3_NO_OVERSAMPLING;
			settings->odr_filter.temp_os = BMP3_NO_OVERSAMPLING;
			settings->odr_filter.odr = BMP3_ODR_25_HZ;

			/* Assign the settings which needs to be set in the sensor */
			settings_sel = BMP3_SEL_PRESS_EN | BMP3_SEL_TEMP_EN |
			BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS | BMP3_SEL_ODR;
			rslt = bmp3_set_sensor_settings(settings_sel, settings, dev);
			if (rslt == BMP3_SENSOR_OK) {
				settings->op_mode = BMP3_MODE_NORMAL;
				rslt = bmp3_set_op_mode(settings, dev);
				if (rslt == BMP3_SENSOR_OK) {
					dev->delay_us(40000, dev->intf_ptr);
					/* Sensor component selection */
					sensor_comp = BMP3_PRESS_TEMP;
					/*
					 * Temperature and Pressure data are read and
					 * stored in the bmp3_data instance
					 */

					rslt = bmp3_get_sensor_data(sensor_comp, &data, dev);
				}
			}
		}
		if (rslt == BMP3_SENSOR_OK) {
			rslt = analyze_sensor_data(&data);
			/* Set the power mode to sleep mode */
			if (rslt == BMP3_SENSOR_OK) {
				settings->op_mode = BMP3_MODE_SLEEP;
				rslt = bmp3_set_op_mode(settings, dev);
			}
		}
	}
	return rslt;
}

/****************** Static Function Definitions *******************************/
/*!
 * @brief  Function to analyze the sensor data
 */
static s8 analyze_sensor_data(const struct bmp3_data *sens_data)
{
	s8 rslt = BMP3_SENSOR_OK;

	if (sens_data->temperature < BMP3_MIN_TEMPERATURE ||
	    sens_data->temperature > BMP3_MAX_TEMPERATURE)
		rslt = BMP3_IMPLAUSIBLE_TEMPERATURE;

	if (rslt == BMP3_SENSOR_OK) {
		if ((sens_data->pressure / 100 < BMP3_MIN_PRESSURE) ||
		    (sens_data->pressure / 100 > BMP3_MAX_PRESSURE))
			rslt = BMP3_IMPLAUSIBLE_PRESSURE;
	}
	return rslt;
}

/*
 * @brief Function to verify the trimming parameters
 */
static s8 validate_trimming_param(struct bmp3_dev *dev)
{
	s8 rslt;
	u8 crc = 0xFF;
	u8 stored_crc;
	u8 trim_param[21];
	u8 i;

	rslt = bmp3_get_regs(BMP3_REG_CALIB_DATA, trim_param, 21, dev);
	if (rslt == BMP3_SENSOR_OK) {
		for (i = 0; i < 21; i++)
			crc = (u8)cal_crc(crc, trim_param[i]);

		crc = (crc ^ 0xFF);
		rslt = bmp3_get_regs(0x30, &stored_crc, 1, dev);
		if (stored_crc != crc)
			rslt = BMP3_TRIMMING_DATA_OUT_OF_BOUND;
	}
	return rslt;
}

/*
 * @brief Function to calculate CRC for the trimming parameters
 */
static s8 cal_crc(u8 seed, u8 data)
{
	s8 poly = 0x1D;
	s8 var2;
	u8 i;

	for (i = 0; i < 8; i++) {
		if ((seed & 0x80) ^ (data & 0x80))
			var2 = 1;
		else
			var2 = 0;
		seed = (seed & 0x7F) << 1;
		data = (data & 0x7F) << 1;
		seed = seed ^ (u8)(poly * var2);
	}
	return (s8)seed;
}

