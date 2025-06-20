/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp3_selftest.h
 * @date		2024-12-04
 * @version		v2.1.0
 *
 */

#ifndef BMP38X_SELFTEST_H_
#define BMP38X_SELFTEST_H_

/*! CPP guard */
#ifdef __cplusplus
extern "C" {
#endif

#include "bmp3.h"

#ifndef BMP3_FLOAT_COMPENSATION

/* 0 degree celsius */
#define BMP3_MIN_TEMPERATURE                                INT16_C(0)

/* 40 degree celsius */
#define BMP3_MAX_TEMPERATURE                                INT16_C(4000)

/* 900 hecto Pascals */
#define BMP3_MIN_PRESSURE                                   UINT32_C(90000)

/* 1100 hecto Pascals */
#define BMP3_MAX_PRESSURE                                   UINT32_C(110000)

#else

/* 0 degree celsius */
#define BMP3_MIN_TEMPERATURE                                (0.0f)

/* 40 degree celsius */
#define BMP3_MAX_TEMPERATURE                                (40.0f)

/* 900 hecto Pascals */
#define BMP3_MIN_PRESSURE                                   (900.0f)

/* 1100 hecto Pascals */
#define BMP3_MAX_PRESSURE                                   (1100.0f)
#endif

/* Error codes for self test  */
#define BMP3_SENSOR_OK                                      UINT8_C(0)
#define BMP3_COMMUNICATION_ERROR_OR_WRONG_DEVICE            UINT8_C(10)
#define BMP3_TRIMMING_DATA_OUT_OF_BOUND                     UINT8_C(20)
#define BMP3_TEMPERATURE_BOUND_WIRE_FAILURE_OR_MEMS_DEFECT  UINT8_C(30)
#define BMP3_PRESSURE_BOUND_WIRE_FAILURE_OR_MEMS_DEFECT     UINT8_C(31)
#define BMP3_IMPLAUSIBLE_TEMPERATURE                        UINT8_C(40)
#define BMP3_IMPLAUSIBLE_PRESSURE                           UINT8_C(41)

/**
 * \ingroup bmp3
 * \defgroup bmp3ApiSelftest Self test
 * @brief Perform self test of sensor
 */

/*!
 * \ingroup bmp3ApiSelftest
 * \page bmp3_api_bmp3_selftest_check bmp3_selftest_check
 * \code
 * int8_t bmp3_selftest_check(const struct bmp3_dev *dev);
 * \endcode
 * @details Self-test API for the BMP38X
 *
 * @param[in] settings : Structure instance of bmp3_settings
 * @param[in]   dev    : Structure instance of bmp3_dev
 *
 * @return Result of API execution status
 * @retval 0  -> Success
 * @retval <0 -> Error
 */
int8_t bmp3_selftest_check(struct bmp3_settings *settings, struct bmp3_dev *dev);

/*! CPP guard */
#ifdef __cplusplus
}
#endif

#endif /* BMP38X_SELFTEST_H_ */

