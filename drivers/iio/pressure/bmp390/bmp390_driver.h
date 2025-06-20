/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp390_driver.h
 * @date		2025-06-02
 * @version		v2.2.0
 *
 * @brief		 BMP390 Linux IIO Driver Header file
 *
 */

#ifndef BMP3_DRIVER_H
#define BMP3_DRIVER_H

#ifdef __cplusplus
extern "C"
{
#endif

/*********************************************************************/
/* System header files */
/*********************************************************************/
#include <linux/types.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <generated/autoconf.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/iio/sw_device.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/syscalls.h>
#include <linux/iio/sw_trigger.h>
#include <linux/err.h>
#include <linux/container_of.h>

/*********************************************************************/
/* Own header files */
/*********************************************************************/
#include "bmp3.h"
#include "bmp3_defs.h"
#include "bmp3_selftest.h"

/*********************************************************************/
/* Macro definitions */
/*********************************************************************/
/** Name of the device driver and IIO device*/
#define SENSOR_NAME "bmp390"

/* Enable interrupt 1 */
#define BMP3_ENABLE_INT1				(1)
/* Maximum number of retries for I2C transfer. */
#define BMP3_MAX_RETRY_I2C_XFER			(10)
/* Delay time for I2C write operations. */
#define BMP3_I2C_WRITE_DELAY_TIME		(1)
/* default sampling frequency - 100Hz */
#define HRTIMER_DEFAULT_SAMPLING_FREQUENCY 100
/* Defines frame count requested
 * As, only Pressure is enabled in this example,
 * Total byte count requested :
 * FIFO_FRAME_COUNT * BMP3_LEN_P_OR_T_HEADER_DATA
 */
#define FIFO_FRAME_COUNT  UINT8_C(50)
/* Maximum FIFO size */
#define FIFO_MAX_SIZE     UINT16_C(1024)
/**
 * enum bmp3_config_func - Enumerations to select the sensors
 */
enum bmp3_config_func {
	BMP3_DATA_READY,
	BMP3_FIFO_FULL,
	BMP3_FIFO_WM
};

/**
 * struct iio_hrtimer_info - High-resolution timer information for IIO
 * @swt: Software trigger associated with the high-resolution timer
 * @timer: High-resolution timer structure
 * @sampling_frequency: Sampling frequency for the timer
 * @period: Timer period in ktime_t format
 */

struct iio_hrtimer_info {
	struct iio_sw_trigger swt;
	struct hrtimer timer;
	unsigned long sampling_frequency;
	ktime_t period;
};

/**
 *	struct bmp3_client_data - Client structure which holds sensor-specific
 *	information.
 */
struct bmp3_client_data {
	struct bmp3_dev device;
	struct device *dev;
	struct iio_trigger *bmp_init;
	struct iio_trigger *feat_input;
	/*! lock: Mutex to protect access to the device data */
	struct mutex lock;
	unsigned int IRQ;
	struct work_struct irq_work;
	struct bmp3_fifo_settings fifo_settings;
	struct bmp3_settings settings;
	struct bmp3_fifo_data fifo;
	struct iio_hrtimer_info *trig_info;
	u8 reg_sel;
	int reg_len;
	const char *name;
	u8 data_ready_en;
	u8 fifo_full_en;
	u8 fifo_wm_en;
	u16 settings_sel;
	u16 settings_fifo;
};

/**
 * struct bmp3_sensor_data - Structure to hold BMP390 sensor data.
 * @temp:  Array to store temperature data as a string.
 * @press: Array to store pressure data as a string.
 * @time:  Array to store timestamp data as a string.
 */
struct bmp3_sensor_data {
	char temp[16];
	char press[16];
	char time[16];
};

/*********************************************************************/
/* Function prototype declarations */
/*********************************************************************/
/*extern the iio_dev of three devices*/
extern struct iio_dev *data_iio_private;
/**
 * bmp390_iio_configure_buffer() - register buffer resources
 * @indo_dev: device instance state
 */
int bmp390_iio_configure_buffer(struct iio_dev *indio_dev);
/**
 * bmp390_iio_unconfigure_buffer() - release buffer resources
 * @indo_dev: device instance state
 */
void bmp390_iio_unconfigure_buffer(struct iio_dev *indio_dev);
/**
 * bmp390_iio_allocate_trigger() - register trigger resources
 * @indo_dev: device instance state
 */
int bmp390_iio_allocate_trigger(struct iio_dev *indio_dev);
/**
 * bmp390_iio_deallocate_trigger() - release trigger resources
 * @indo_dev: device instance state
 */
void bmp390_iio_deallocate_trigger(struct iio_dev *indio_dev);
/**
 * bmp3_probe - This is the probe function for bmp3 sensor.
 * Called from the I2C driver probe function to initialize the sensor.
 *
 * @client_data : Structure instance of client data.
 * @dev : Structure instance of device.
 *
 * Return : Result of execution status
 * * 0 - Success
 * * negative value -> Error
 */
int bmp3_probe(struct iio_dev *data_iio_private);

/**
 * bmp3_suspend - This function puts the driver
 * and device to suspend mode.
 *
 * @dev : Structure instance of device.
 *
 * Return : Result of execution status
 * * 0 - Success
 * * negative value -> Error
 */
int bmp3_suspend(struct device *dev);

/**
 * bmp3_resume - This function is used to bring back
 * device from suspend mode.
 *
 * @dev	 : Structure instance of device.
 *
 * Return : Result of execution status
 * * 0 - Success
 * * negative value -> Error
 */
int bmp3_resume(struct device *dev);

/**
 * bmp3_remove - This function removes the driver from the device.
 *
 * @dev : Structure instance of device.
 *
 * Return : Result of execution status
 * * 0 - Success
 */
void bmp3_remove(struct iio_dev *data_iio_private);

#ifdef __cplusplus
}
#endif

#endif /* BMP3_DRIVER_H_	*/

