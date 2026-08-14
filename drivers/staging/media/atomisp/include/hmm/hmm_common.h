/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
 */

#ifndef	__HMM_BO_COMMON_H__
#define	__HMM_BO_COMMON_H__

#define	HMM_BO_NAME	"HMM"

/*
 * some common use micros
 */
#define	var_equal_return(var1, var2, exp, fmt, arg ...)	\
	do { \
		if ((var1) == (var2)) { \
			dev_err(atomisp_dev, \
			fmt, ## arg); \
			return exp;\
		} \
	} while (0)

#define	var_equal_return_void(var1, var2, fmt, arg ...)	\
	do { \
		if ((var1) == (var2)) { \
			dev_err(atomisp_dev, \
			fmt, ## arg); \
			return;\
		} \
	} while (0)

#endif
