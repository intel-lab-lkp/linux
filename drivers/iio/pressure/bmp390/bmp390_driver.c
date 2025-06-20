// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp390_driver.c
 * @date		2025-06-02
 * @version		v2.2.0
 *
 * @brief		 BMP390 Linux IIO Driver
 *
 */
/*********************************************************************/
/* Own header files */
/*********************************************************************/
#include "bmp390_driver.h"
/*********************************************************************/
/* System header files */
/*********************************************************************/

#include <linux/string.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/math64.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>

/*********************************************************************/
/* Local macro definitions */
/*********************************************************************/
/*! define driver version */
#define DRIVER_VERSION "2.2.0"
/*********************************************************************/
/* Global data */
/*********************************************************************/
/*! define millisecs to microsecs conversion */
#define MS_TO_US(msec)		UINT32_C((msec) * 1000)
/*! define max check times for chip id */
#define CHECK_CHIP_ID_TIME_MAX		UINT8_C(5)
/*! define sensor i2c address */
#define BMP_I2C_ADDRESS			BMP3_ADDR_I2C_SEC
/*! define max I2C packet size */
#define I2C_PACKET_MAX_SIZE             UINT16_C(256)
/*! define interrupt GPIO */
#define BMP_GPIO_ID			UINT8_C(59)
/*! define default delay time used by input event [unit:ms] */
#define BMP_DELAY_DEFAULT		UINT16_C(200)
/*! no action to selftest */
#define BMP_SELFTEST_NO_ACTION		INT8_C(-1)

/*! define max chars to print using scnprintf */
#define MAX_CHARS			UINT8_C(128)
/*! FIFO temperature pressure header frame */
#define FIFO_TEMP_PRESS_FRAME		UINT8_C(0x94)
/*! FIFO temperature header frame */
#define FIFO_TEMP_FRAME			UINT8_C(0x90)
/*! FIFO pressure header frame */
#define FIFO_PRESS_FRAME		UINT8_C(0x84)
/*! FIFO time header frame */
#define FIFO_TIME_FRAME			UINT8_C(0xA0)
/*! Converts milliseconds to nanoseconds */
#define MS_TO_NS(msec)		((msec) * 1000 * 1000)
/*! Converts milliseconds to microseconds */
#define MS_TO_US(msec)		UINT32_C((msec) * 1000)

/*Temperature data register address*/
#define BMP3_REG_TEMP_DATA		UINT8_C(0x07)

/* IIO Module */
static const struct iio_event_spec bmp390_event = {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_shared_by_type = BIT(IIO_EV_INFO_VALUE) |
				BIT(IIO_EV_INFO_ENABLE)
};

/**
 * BMP390_CHANNELS_CONFIG - Macro to configure BMP390 sensor channels
 * @device_type: Type of the device (e.g., IIO_PRESSURE)
 * @si: Scan index for the channel
 * @mod: Modifier for the channel (e.g., IIO_MOD_X)
 * @addr: Address of the channel
 *
 * This macro sets up the configuration for a BMP390 sensor channel, including
 * the type, scan index, modifier, address, and scan type details. It also
 * specifies the event specification and the number of event specifications.
 *
 * The scan type details include:
 * - sign: Sign of the data ('s' for signed)
 * - realbits: Number of valid data bits
 * - shift: Bit shift applied to the data
 * - storagebits: Number of storage bits
 * - endianness: Endianness of the data (IIO_LE for little-endian)
 *
 * The event specification is set to bmp390_event, and the number of event
 * specifications is set to 1.
 */
#define BMP390_CHANNELS_CONFIG(device_type, si, mod, addr) \
	{ \
		.type = device_type, \
		.modified = 1, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),				\
		.scan_index = si, \
		.channel2 = mod, \
		.address = addr, \
		.scan_type = { \
			.sign = 's', \
			.realbits = 64, \
			.shift = 0, \
			.storagebits = 64, \
			.endianness = IIO_LE, \
		}, \
	.event_spec = &bmp390_event,					\
	.num_event_specs = 1		\
	}

#define BMP390_BYTE_FOR_PER_AXIS_CHANNEL		2

/* scan element definition */
enum BMP390_AXIS_SCAN {
	BMP390_SCAN_TEMP,
	BMP390_SCAN_PRESS,
	BMP390_SCAN_TIMESTAMP,
};

/*iio chan spec for  BMP390 sensor*/
static const struct iio_chan_spec bmp390_iio_channels[] = {
/*acc channel*/
/*lint -e446*/
BMP390_CHANNELS_CONFIG(IIO_TEMP, BMP390_SCAN_TEMP,
		       IIO_MOD_TEMP_OBJECT, BMP3_REG_TEMP_DATA),
BMP390_CHANNELS_CONFIG(IIO_PRESSURE, BMP390_SCAN_PRESS,
		       IIO_MOD_TEMP_OBJECT, BMP3_REG_DATA),
/*lint +e446*/
/*ap timestamp channel*/
IIO_CHAN_SOFT_TIMESTAMP(BMP390_SCAN_TIMESTAMP)

};

/**
 * bmp3_config_func_name - Array of strings representing the names of different
 *                         BMP390 configuration functions.
 *                         These functions include:
 *                         - "data ready interrupt"
 *                         - "fifo full interrupt"
 *                         - "fifo water mark interrupt"
 */
static char *bmp3_config_func_name[] = {
	"data ready interrupt",
	"fifo full interrupt",
	"fifo water mark interrupt"
};

/**
 * bmp3_config_en_disable - Array of strings representing the enable/disable
 *                          states for BMP390 configuration.
 *                          These states include:
 *                          - "disabled"
 *                          - "enabled"
 */
static char *bmp3_config_en_disable[] = {
	"disabled",
	"enabled"
};

/**
 * bmp3_delay_us - Adds a delay in units of microsecs.
 *
 * @usec: Delay value in microsecs.
 */
static void bmp3_delay_us(u32 usec, void *intf_ptr)
{
	if (usec <= (MS_TO_US(20))) {
		/* Delay range of usec to usec + 1 millisecs
		 * required due to kernel limitation
		 */
		usleep_range(usec, usec + 1000);
	} else {
		msleep(usec / 1000);
	}
}

/**
 * check_error -
 * check error code and print error message if err is non 0.
 *
 * @print_msg	: print message to print on if err is not 0.
 * @err			: error code return to be checked.
 */
static void check_error(char *print_msg, int err)
{
	if (err)
		pr_err("%s failed with return code:%d\n", print_msg, err);
}

/**
 * chip_id_show - sysfs read callback for reading the
 * chip id of the sensor.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t chip_id_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	u8 chip_id = {0};
	int ret;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	ret = bmp3_get_regs(BMP3_REG_CHIP_ID, &chip_id, 1,
			    &client_data->device);
	check_error("read chip id register", ret);
	return scnprintf(buf, 96, "chip_id=0x%x\n", chip_id);
}

/**
 * avail_sensor_show -
 * sysfs read callback which prints sensor supported by
 * this current driver.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t avail_sensor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, 32, "bmp390\n");
}

/*!
 * @brief sysfs write callback which performs the iio generic buffer test
 *
 * @param[in] dev	: Device instance.
 * @param[in] attr	: Instance of device attribute file.
 * @param[in] buf	: Instance of the data buffer which serves as input.
 * @param[in] count : Number of characters in the buffer `buf`.
 *
 * @return Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t iio_generic_buffer_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rslt;
	unsigned long iio_test;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	/* Base of decimal number system is 10 */
	rslt = kstrtoul(buf, 10, &iio_test);

	if (rslt) {
		pr_err("iio_generic_buffer : invalid input");
		return -EIO;
	}

	if (iio_test) {
		/*lint -e534*/
		bmp390_iio_allocate_trigger(input);
		/*lint +e534*/
	} else {
		bmp390_iio_deallocate_trigger(input);
		/*lint -e534*/
		bmp3_soft_reset(&client_data->device);
		/*lint +e534*/
	}

	return count;
}

/**
 * reg_sel_show - sysfs read callback which reads register
 * address and length selected for a read/write operation via reg_val.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t reg_sel_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	return scnprintf(buf, 64, "reg=0X%02X, len=%d\n",
		client_data->reg_sel, client_data->reg_len);
}

/**
 * reg_sel_store - sysfs write callback which sets register address
 * and length to be selected for a read/write operation via reg_val.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t reg_sel_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	int ret;
	unsigned int sdata[2] = {0};

	mutex_lock(&client_data->lock);
	ret = sscanf(buf, "%11X %11d", &sdata[0], &sdata[1]);
	client_data->reg_sel = (u8)sdata[0];
	client_data->reg_len = (u8)sdata[1];
	if (ret != 2 || client_data->reg_len > 128 || client_data->reg_sel > 127) {
		pr_err("Invalid argument");
		mutex_unlock(&client_data->lock);
		return -EINVAL;
	}
	mutex_unlock(&client_data->lock);
	return count;
}

/**
 * reg_val_show - sysfs read callback which read sensor data register
 * address and length set via reg_sel.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t reg_val_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	int ret;
	u8 reg_data[128];
	int i;
	int pos;

	if (client_data->reg_len == 0) {
		pr_err("reg_sel length can't be 0, please set reg_sel first");
		return -EINVAL;
	}
	mutex_lock(&client_data->lock);
	ret = bmp3_get_regs(client_data->reg_sel,
			    reg_data,
			    client_data->reg_len,
			    &client_data->device);

	check_error("reg_val read", ret);
	pos = 0;
	for (i = 0; i < client_data->reg_len; ++i) {
		pos += scnprintf(buf + pos, 16, "%02X", reg_data[i]);
		buf[pos++] = (i + 1) % 16 == 0 ? '\n' : ' ';
	}
	mutex_unlock(&client_data->lock);
	if (buf[pos - 1] == ' ')
		buf[pos - 1] = '\n';
	return pos;
}

/**
 * reg_val_store - sysfs write callback which write sensor data register
 * address and length set via reg_sel.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t reg_val_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	int ret;
	u8 reg_data[128] = {0,};
	int i, j, status, digit;

	if (client_data->reg_len == 0) {
		pr_err("reg_sel length can't be 0, please set reg_sel values first\n");
		return -EINVAL;
	}
	status = 0;
	mutex_lock(&client_data->lock);
	/* Lint -save -e574 */
	for (i = j = 0; i < count && j < client_data->reg_len; ++i) {
		/* Lint -restore */
		if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t' ||
		    buf[i] == '\r') {
			status = 0;
			++j;
			continue;
		}
		digit = buf[i] & 0x10 ? (buf[i] & 0xF) : ((buf[i] & 0xF) + 9);
		pr_info("digit is %d\n", digit);
		switch (status) {
		case 2:
			++j;
			reg_data[j] = digit;
			status = 1;
			break;
		case 0:
			reg_data[j] = digit;
			status = 1;
			break;
		case 1:
			reg_data[j] = reg_data[j] * 16 + digit;
			status = 2;
			break;
		}
	}
	if (status > 0)
		++j;
	if (j > client_data->reg_len) {
		j = client_data->reg_len;
	} else if (j < client_data->reg_len) {
		pr_err("Invalid argument\n");
		mutex_unlock(&client_data->lock);
		return -EINVAL;
	}
	pr_info("Reg data read as\n");
	for (i = 0; i < j; ++i)
		pr_info("%d\n", reg_data[i]);
	ret = client_data->device.write(client_data->reg_sel, reg_data,
					client_data->reg_len,
					client_data->device.intf_ptr);
	mutex_unlock(&client_data->lock);
	check_error("reg_val write", ret);
	return count;
}

/**
 * driver_version_show - sysfs read callback which
 * provides current driver version.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t driver_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, 128,
		"Driver version: %s\n", DRIVER_VERSION);
}

/**
 * op_mode_show - Show the current operating mode of the BMP390 sensor.
 * @dev: The device object.
 * @attr: The device attribute.
 * @buf: The buffer to store the operating mode string.
 *
 * This function retrieves the current operating mode of the BMP390 sensor
 * and stores it in the provided buffer. If the operation is
 * successful, it returns the number of characters written to the buffer.
 * Otherwise, it returns an error code.
 *
 * Return: Number of characters written to the buffer on success,
 * or -EIO on failure.
 */
static ssize_t op_mode_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	u8 op_mode;
	s16 status;

	mutex_lock(&client_data->lock);
	status = bmp3_get_op_mode(&op_mode, &client_data->device);
	mutex_unlock(&client_data->lock);

	if (status == BMP3_OK)
		return scnprintf(buf, MAX_CHARS, "op_mode = %u\n", op_mode);
	else
		return -EIO;
}

/*!
 * @brief Sets the operating mode via a sysfs node.
 *
 * This function allows the user to set the operating mode of the BMP390 sensor
 * by writing to a sysfs node. The input value must be a valid operating mode
 * (0 to 3). If the input value is invalid, an error message is printed and
 * the function returns an error code.
 *
 * @param dev Pointer to the device structure.
 * @param attr Pointer to the device attribute structure.
 * @param buf Buffer containing the input value as a string.
 * @param count Size of the input buffer.
 * @return The number of bytes written on success,
 * or a negative error code on failure.
 */
static ssize_t op_mode_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	unsigned long  op_mode;
	int ret;

	ret = kstrtoul(buf, 10, &op_mode);
	if (op_mode > 3) {
		pr_err("Invalid input\nusage:\n");
		pr_err("echo op_mode > power_mode\n");
		return -EIO;
	}
	mutex_lock(&client_data->lock);
	client_data->settings.op_mode = op_mode;
	ret = bmp3_set_op_mode(&client_data->settings, &client_data->device);
	mutex_unlock(&client_data->lock);

	if (ret == BMP3_OK)
		return count;

	return ret;
}

/**
 * odr_show - Show the output data rate (ODR) of the BMP390 sensor.
 * @dev: The device from which to retrieve the ODR.
 * @attr: The device attribute (unused).
 * @buf: The buffer to store the ODR value.
 *
 * This function retrieves the output data rate (ODR) of the BMP390 sensor
 * and stores it in the provided buffer. If the sensor settings
 * are successfully retrieved, the ODR value is formatted and stored in the
 * buffer. Otherwise, an error code is returned.
 *
 * Return: The number of characters written to the buffer on success,
 *         or a negative error code on failure.
 */
static ssize_t odr_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	unsigned int odr;
	s8 status;

	mutex_lock(&client_data->lock);
	status = bmp3_get_sensor_settings(&client_data->settings,
					  &client_data->device);
	odr = client_data->settings.odr_filter.odr;
	mutex_unlock(&client_data->lock);

	if (status == 0)
		return scnprintf(buf, MAX_CHARS, "%d\n", odr);

	return -EIO;
}

/**
 * odr_store - Store the output data rate (ODR) value for the BMP390 sensor.
 * @dev: The device structure.
 * @attr: The device attribute structure.
 * @buf: The buffer containing the ODR value as a string.
 * @count: The size of the buffer.
 *
 * This function parses the ODR value from the input buffer, validates it,
 * and updates the sensor settings accordingly.
 *
 * Return: The number of bytes processed on success, or a negative error code
 * on failure.
 */
static ssize_t odr_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	unsigned long odr;
	u16 settings_sel;
	int ret;

	ret = kstrtoul(buf, 10, &odr);
	check_error("odr input receive", ret);
	if (ret) {
		pr_err("odr invalid input:%ld\n", odr);
		return -EINVAL;
	}
	settings_sel = BMP3_SEL_ODR;
	mutex_lock(&client_data->lock);
	client_data->settings.odr_filter.odr = odr;
	ret = bmp3_set_sensor_settings(settings_sel, &client_data->settings,
				       &client_data->device);
	mutex_unlock(&client_data->lock);
	if (ret == BMP3_OK)
		return count;

	return ret;
}

/**
 * sensor_conf_show - sysfs read callback which reads
 * pressure sensor configuration parameters.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t sensor_conf_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;
	struct bmp3_settings settings = { 0 };
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	mutex_lock(&client_data->lock);
	ret = bmp3_get_sensor_settings(&settings,
				       &client_data->device);
	mutex_unlock(&client_data->lock);
	check_error("bmp3_get_sensor_settings", ret);

	return scnprintf(buf, PAGE_SIZE,
					"cfg.odr_filter.odr:%d\ncfg.press_en:%d\ncfg.temp_en:%d\n",
					settings.odr_filter.odr,
					settings.press_en,
					settings.temp_en);
}

/**
 * sensor_conf_store - sysfs write callback which sets
 * pressure sensor configuration parameters.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t sensor_conf_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned int sdata[2] = {0};
	u16 settings_sel = 0;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	ret = sscanf(buf, "%11d %11d", &sdata[0], &sdata[1]);
	if (ret != 2) {
		pr_err("Invalid input\nusage:\n");
		pr_err("echo odr press_enable/disable > sensor_conf\n");
		return -EIO;
	}
	mutex_lock(&client_data->lock);
	ret = bmp3_get_sensor_settings(&client_data->settings,
				       &client_data->device);
	check_error("bmp3_get_sensor_settings", ret);
	client_data->settings.odr_filter.odr = (u8)sdata[0];
	client_data->settings.press_en = (u8)sdata[1];
	client_data->settings.temp_en = BMP3_ENABLE;
	settings_sel |= BMP3_SEL_PRESS_EN | BMP3_SEL_PRESS_OS;

	settings_sel |= BMP3_SEL_TEMP_EN | BMP3_SEL_TEMP_OS;
	client_data->settings.odr_filter.temp_os = BMP3_NO_OVERSAMPLING;

	settings_sel |= BMP3_SEL_ODR;

	ret = bmp3_set_sensor_settings(settings_sel, &client_data->settings,
				       &client_data->device);
	mutex_unlock(&client_data->lock);
	check_error("bmp3_set_sensor_settings", ret);
	return count;
}

/**
 * fifo_conf_show - sysfs read callback which reads
 * fifo sensor configuration parameters.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t fifo_config_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;
	struct bmp3_fifo_settings fifo_settings = { 0 };
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	mutex_lock(&client_data->lock);
	ret = bmp3_get_fifo_settings(&fifo_settings, &client_data->device);
	check_error("bmp3_get_fifo_settings", ret);
	mutex_unlock(&client_data->lock);

	return scnprintf(buf, PAGE_SIZE,
					"mode:%d\nffull:%d\nfwtm:%d\n"
					"fifo.press_en:%d\nfifo.temp_en:%d\nfifo.time_en:%d\n",
					fifo_settings.mode,
					fifo_settings.ffull_en,
					fifo_settings.fwtm_en,
					fifo_settings.press_en,
					fifo_settings.temp_en,
					fifo_settings.time_en);
}

/**
 * fifo_conf_store - sysfs write callback which sets
 * fifo configuration parameters.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t fifo_config_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int ret;
	unsigned int sdata[2] = {0};
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	client_data->settings_sel = 0;
	client_data->settings_fifo = 0;
	ret = sscanf(buf, "%11d %11d", &sdata[0], &sdata[1]);
	if (ret != 2) {
		pr_err("Invalid input\nusage:\n");
		pr_err("echo press_en/dis time_en/dis > fifo_conf\n");
		return -EIO;
	}
	if (sdata[0] > 1) {
		pr_err("Invalid input\nusage:\n");
		pr_err("pressure be 0 or 1\n");
		pr_err("0 : pressure disable\n 1 : pressure enable\n");
		return -EIO;
	}
	if (sdata[1] > 1) {
		pr_err("Invalid input\nusage:\n");
		pr_err("time be 0 or 1\n");
		pr_err("0 : time disable\n 1 : time enable\n");
		return -EIO;
	}
	mutex_lock(&client_data->lock);
	client_data->fifo_settings.press_en = 0;
	client_data->fifo_settings.temp_en = 0;
	client_data->fifo_settings.time_en = 0;
	client_data->fifo_settings.filter_en = BMP3_ENABLE;
	client_data->fifo_settings.down_sampling = BMP3_FIFO_NO_SUBSAMPLING;
	client_data->fifo.req_frames = FIFO_FRAME_COUNT;
	client_data->fifo.byte_count = FIFO_MAX_SIZE;
	client_data->settings_fifo = BMP3_SEL_FIFO_MODE |
	BMP3_SEL_FIFO_DOWN_SAMPLING | BMP3_SEL_FIFO_FILTER_EN;
	client_data->settings_fifo |= BMP3_SEL_FIFO_FWTM_EN |
	BMP3_SEL_FIFO_FULL_EN;

	client_data->fifo_settings.press_en = (u8)sdata[0];
	client_data->settings.press_en = (u8)sdata[0];

	client_data->fifo_settings.temp_en = BMP3_ENABLE;
	client_data->settings.temp_en = BMP3_ENABLE;

	client_data->fifo_settings.time_en = (u8)sdata[1];

	client_data->settings_sel |= BMP3_SEL_ODR;

	client_data->settings_sel |= BMP3_SEL_PRESS_EN | BMP3_SEL_PRESS_OS;

	client_data->settings_fifo |= BMP3_SEL_FIFO_PRESS_EN;

	client_data->settings_sel |=  BMP3_SEL_TEMP_EN |  BMP3_SEL_TEMP_OS;

	client_data->settings_fifo |= BMP3_SEL_FIFO_TEMP_EN;
	client_data->settings_fifo |= BMP3_SEL_FIFO_TIME_EN;

	ret = bmp3_set_sensor_settings(client_data->settings_sel,
				       &client_data->settings,
				       &client_data->device);
	check_error("bmp3_set_fifo_settings", ret);
	ret = bmp3_set_fifo_settings(client_data->settings_fifo,
				     &client_data->fifo_settings, &client_data->device);
	check_error("bmp3_set_fifo_settings", ret);
	mutex_unlock(&client_data->lock);
	check_error("bmp3_set_sensor_settings", ret);
	return count;
}

/**
 * soft_reset_store - sysfs write callback which
 * performs sensor soft reset
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t soft_reset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;
	unsigned long soft_reset;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	ret = kstrtoul(buf, 10, &soft_reset);
	check_error("soft_reset node input receive", ret);
	if (soft_reset) {
		ret = bmp3_soft_reset(&client_data->device);
		check_error("bmp3_soft_reset", ret);
		if (ret != BMP3_OK)
			pr_err("Soft reset failed\n");
		else
			pr_info("Soft reset success\n");
	} else {
		pr_err("Invalid Input\nusage: echo 1 > softreset\n");
	}
	return count;
}

/**
 * sensor_data_show - sysfs read callback which reads
 * sensor data registers.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t sensor_data_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int ret;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	struct bmp3_data comp_data;
	struct bmp3_status status = { { 0 } };

	ret = bmp3_get_status(&status, &client_data->device);
	check_error("bmp3_get_status", ret);
	mutex_lock(&client_data->lock);
	ret = bmp3_get_sensor_data(BMP3_PRESS_TEMP, &comp_data,
				   &client_data->device);
	mutex_unlock(&client_data->lock);
	check_error("bmp3_get_sensor_data", ret);
	comp_data.pressure = (client_data->settings.press_en == BMP3_DISABLE)
						? 0 : comp_data.pressure;
	comp_data.temperature =
						(client_data->settings.temp_en == BMP3_DISABLE)
						? 0 : comp_data.temperature;

	return scnprintf(buf, PAGE_SIZE,
					"Temperature: %lld deg C,	Pressure:  %llu\n",
					comp_data.temperature, comp_data.pressure);
}

/**
 * config_function_show - sysfs read callback which gives the list of
 * enabled sensor features.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t config_function_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	return scnprintf(buf, PAGE_SIZE,
		"drdy%d=%d\nfifo_full%d=%d\nfifo_wm%d=%d\n",
		BMP3_DATA_READY, client_data->data_ready_en,
		BMP3_FIFO_FULL, client_data->fifo_full_en,
		BMP3_FIFO_WM, client_data->fifo_wm_en);
}

/**
 * bmp390_sensor_feature_config - function to enable or
 * disable sensor feature.
 *
 * @client_data: client structure that holds device instance
 *				and parameters
 * @config_func: feature to be configured
 * @enable:		 input to enable or disable selected feature.
 *
 * Return: 0 if success.
 */
static ssize_t bmp3_sensor_feature_config(struct bmp3_client_data *client_data,
					  int config_func, int enable)
{
	int ret;

	if ((config_func < BMP3_DATA_READY || config_func > BMP3_FIFO_WM) &&
	    (enable != 1 || enable != 0)) {
		pr_err("Invalid input passed for sensor feature configuration\n");
		return -EINVAL;
	}
	switch (config_func) {
	case BMP3_DATA_READY:
		client_data->settings.int_settings.drdy_en = BMP3_ENABLE;
		client_data->settings.press_en = BMP3_ENABLE;
		client_data->settings.temp_en = BMP3_ENABLE;
		client_data->settings.odr_filter.press_os = BMP3_OVERSAMPLING_2X;
		client_data->settings.odr_filter.temp_os = BMP3_OVERSAMPLING_2X;
		client_data->settings_sel = BMP3_SEL_PRESS_EN | BMP3_SEL_TEMP_EN
					| BMP3_SEL_PRESS_OS | BMP3_SEL_TEMP_OS |
					BMP3_SEL_ODR | BMP3_SEL_DRDY_EN;
		client_data->data_ready_en = enable;
		ret = bmp3_set_sensor_settings(client_data->settings_sel,
					       &client_data->settings,
					       &client_data->device);
		check_error("bmp3_set_sensor_settings", ret);
		break;
	case BMP3_FIFO_FULL:
		client_data->fifo_settings.mode = enable;
		client_data->fifo_settings.ffull_en = enable;
		ret = bmp3_set_fifo_settings(client_data->settings_fifo,
					     &client_data->fifo_settings,
					     &client_data->device);
		check_error("bmp3_set_fifo_settings", ret);
		client_data->fifo_full_en = enable;
		client_data->settings.odr_filter.press_os = BMP3_NO_OVERSAMPLING;
		client_data->settings.odr_filter.temp_os = BMP3_NO_OVERSAMPLING;
		ret = bmp3_set_sensor_settings(client_data->settings_sel,
					       &client_data->settings,
					       &client_data->device);
		check_error("bmp3_set_fifo_settings", ret);
		break;
	case BMP3_FIFO_WM:
		client_data->fifo_settings.mode = enable;
		client_data->fifo_settings.fwtm_en = enable;
		ret = bmp3_set_fifo_settings(client_data->settings_fifo,
					     &client_data->fifo_settings,
					     &client_data->device);
		check_error("bmp3_set_fifo_settings", ret);
		if (client_data->fifo_settings.fwtm_en == BMP3_ENABLE) {
			ret = bmp3_set_fifo_watermark(&client_data->fifo,
						      &client_data->fifo_settings,
						      &client_data->device);
			check_error("bmp3_set_fifo_watermark", ret);
		}
		client_data->fifo_wm_en = enable;
		client_data->settings.odr_filter.press_os = BMP3_OVERSAMPLING_2X;
		client_data->settings.odr_filter.temp_os = BMP3_OVERSAMPLING_2X;
		ret = bmp3_set_sensor_settings(client_data->settings_sel,
					       &client_data->settings,
					       &client_data->device);
		check_error("bmp3_set_fifo_settings", ret);
		break;
	default:
		pr_err("Invalid sensor handle: %d\n", config_func);
		return -EINVAL;
	}

	if (ret) {
		pr_err("set int map failed with ret code : %d\n", ret);
		return -EIO;
	}
	pr_info("%s %s\n", bmp3_config_func_name[config_func],
		bmp3_config_en_disable[enable]);
	/*lint -e534*/
	schedule_work(&client_data->irq_work);
	/*lint -e534*/
	return ret;
}

/**
 * config_function_store - sysfs write callback which enable or disable
 * the sleected sensor feature.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as input.
 * @count: Number of characters in the buffer `buf`.
 *
 * Return: Number of characters used from buffer `buf`,
 * which equals count.
 */
static ssize_t config_function_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	int config_func = 0;
	int enable = 0;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	ret = sscanf(buf, "%11d %11d", &config_func, &enable);
	if (ret != 2) {
		pr_err("Invalid argument\nusage:\n");
		pr_err("echo sensor_feature en_disable > config_function\n");
		return -EINVAL;
	}
	ret = bmp3_sensor_feature_config(client_data, config_func, enable);
	check_error("config function en/disable", ret);
	return count;
}

/**
 * fifo_data_frame_print - sysfs read callback which reads the fifo data
 * from the sensor.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t fifo_data_frame_print(struct bmp3_client_data *client_data)
{
	struct bmp3_data *sensor_data;
	u16 data_size = 0;
	s8 rslt = 0;
	u8 index = 0;
	u16 fifo_length = 0;
	u16 watermark = 0;
	u8 *fifo_data;

	mutex_lock(&client_data->lock);
	fifo_data = kzalloc(FIFO_MAX_SIZE * FIFO_MAX_SIZE, GFP_KERNEL);
	if (!fifo_data) {
		mutex_unlock(&client_data->lock);
		pr_err("%s memory error %d.\n", SENSOR_NAME, rslt);
		return -ENOMEM;
	}
	rslt = bmp3_get_fifo_length(&fifo_length, &client_data->device);
	check_error("bmp3_get_fifo_length", rslt);
	if (client_data->fifo_wm_en == 1) {
		rslt = bmp3_get_fifo_watermark(&watermark, &client_data->device);
		check_error("bmp3_get_fifo_watermark", rslt);
	}
	client_data->fifo.buffer = fifo_data;
	client_data->fifo.req_frames = FIFO_FRAME_COUNT;
	rslt = bmp3_get_fifo_data(&client_data->fifo,
				  &client_data->fifo_settings,
				  &client_data->device);
	check_error("bmp3_get_fifo_data", rslt);
	client_data->fifo.req_frames = client_data->fifo.byte_count;
	data_size = client_data->fifo.req_frames * sizeof(struct bmp3_data);

	sensor_data = kzalloc(sizeof(u8) * FIFO_MAX_SIZE * 16, GFP_KERNEL);
	if (!sensor_data) {
		mutex_unlock(&client_data->lock);
		pr_err("%s memory error %d.\n", SENSOR_NAME, rslt);
		kfree(fifo_data);
		return -ENOMEM;
	}
	if (rslt == BMP3_OK) {
		if (client_data->fifo_wm_en == 1) {
			pr_info("Fifo Watermark\n");
			pr_info("Watermark level : %d\n", watermark);
		}
		pr_info("Available fifo length : %d\n", fifo_length);
		pr_info("Fifo byte count from fifo structure : %d\n",
			client_data->fifo.byte_count);
		pr_info("FIFO frames requested : %d\n",
			client_data->fifo.req_frames);

		rslt = bmp3_extract_fifo_data(sensor_data, &client_data->fifo,
					      &client_data->device);
		check_error("bmp3_extract_fifo_data", rslt);
		data_size =
		client_data->fifo.parsed_frames * sizeof(struct bmp3_data);

		pr_info("FIFO frames extracted : %d\n",
			client_data->fifo.parsed_frames);

		for (index = 0; index < client_data->fifo.parsed_frames; index++) {
			pr_info("Frame[%d] , P : %llu, T :%lld deg C, Time : : %llu\n",
				index, sensor_data[index].pressure,
				sensor_data[index].temperature,
				(u64)client_data->fifo.sensor_time);
		}
		data_size = 0;
	}
	mutex_unlock(&client_data->lock);
	kfree(fifo_data);
	kfree(sensor_data);
	return data_size;
}

/*!
 * @brief get fifo data via sysfs node
 */
static ssize_t fifo_data_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	u16 rslt;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);

	rslt = fifo_data_frame_print(client_data);
	check_error("fifo_data_frame_print", rslt);

	return rslt;
}

/**
 * self_test_show - sysfs read callback which gives the
 * sensor low gas and high gas variant self test result.
 *
 * @dev: Device instance
 * @attr: Instance of device attribute file
 * @buf: Instance of the data buffer which serves as output.
 *
 * Return: Number of characters returned.
 */
static ssize_t self_test_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int err;
	unsigned long self_test;
	struct iio_dev *input = dev_to_iio_dev(dev);
	struct bmp3_client_data *client_data = iio_priv(input);
	struct bmp3_settings settings = { 0 };

	err = kstrtoul(buf, 10, &self_test);
	check_error("self test node input receive", err);
	if (self_test) {
		mutex_lock(&client_data->lock);
		err = bmp3_selftest_check(&settings,
					  &client_data->device);
		check_error("bmp3_selftest_check", err);
		mutex_unlock(&client_data->lock);
		if (err) {
			pr_err("selftest failed with return code : %d\n", err);
			return -EIO;
		}
		pr_info("self test successful\n");
	}
	return count;
}

/**
 * bmp390_read_axis_data - Reads axis data from the BMP390 sensor.
 * @indio_dev: The IIO device structure.
 * @reg_address: The register address to read data from.
 * @data: Pointer to store the read data.
 *
 * This function reads either temperature or pressure data from the BMP390
 * sensor based on the provided register address.
 * It uses the bmp3_get_sensor_data function to retrieve the sensor data and
 * stores the result in the provided data pointer.
 *
 * Return: 0 on success, a negative error code otherwise.
 */

static int bmp390_read_axis_data(struct iio_dev *indio_dev,
				 u8 reg_address,
				 s32 *axis_data)
{
	int ret;
	struct bmp3_data comp_data;
	struct bmp3_client_data *client_data = iio_priv(indio_dev);

	ret = bmp3_get_sensor_data(BMP3_PRESS_TEMP, &comp_data,
				   &client_data->device);
	check_error("bmp3_get_sensor_data", ret);
	if (ret < 0)
		return ret;
	if (reg_address == BMP3_REG_TEMP_DATA)
		*axis_data = comp_data.temperature;

	else
		*axis_data = (s32)comp_data.pressure;

	return 0;
}

/**
 * bmp390_read_raw - Read raw data from the BMP390 sensor
 * @indio_dev: The IIO device structure
 * @ch: The IIO channel specification
 * @val: Pointer to store the raw value
 * @val2: Pointer to store the second raw value (unused)
 * @mask: The mask specifying the type of data to read
 *
 * This function reads raw data from the BMP390 sensor based on the specified
 * channel type and mask. It supports reading temperature and pressure data.
 *
 * Return: IIO_VAL_INT on success, negative error code on failure.
 */
static int bmp390_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *ch, int *val,
			   int *val2, long mask)
{
	int ret, result;
	s32 tval = 0;
	struct bmp3_client_data *client_data = iio_priv(indio_dev);

	switch (mask) {
	case 0:
	{
		result = 0;
		ret = IIO_VAL_INT;
		mutex_lock(&client_data->lock);
		switch (ch->type) {
		case IIO_TEMP:
			result = bmp390_read_axis_data(indio_dev,
						       ch->address, &tval);
			*val = tval;
			break;
		case IIO_PRESSURE:
			result = bmp390_read_axis_data(indio_dev,
						       ch->address, &tval);
			*val = tval;
			break;
		default:
			ret = -EINVAL;
			break;
		}
	mutex_unlock(&client_data->lock);
	if (result < 0)
		return result;
	return ret;
	}

	default:
		return -EINVAL;
	}
}

/**
 * This file defines various IIO device attributes for the BMP390 sensor driver.
 * Each attribute is associated with specific permissions and corresponding
 * show and store functions for reading and writing the attribute values.
 *
 * Attributes:
 * - chip_id: Read-only attribute to show the chip ID.
 * - reg_val: Read-write attribute to show and store register values.
 * - reg_sel: Read-write attribute to show and store register selection.
 * - config_function: Read-write attribute to show and store
 * configuration functions.
 * - sensor_data: Read-only attribute to show sensor data.
 * - soft_reset: Write-only attribute to call soft reset.
 * - sensor_conf: Read-write attribute to show and store sensor configuration.
 * - driver_version: Read-only attribute to show the driver version.
 * - avail_sensor: Read-only attribute to show available sensors.
 * - op_mode: Read-write attribute to show and store operation mode.
 * - odr: Read-write attribute to show and store output data rate.
 * - fifo_data: Read-only attribute to show FIFO data.
 * - fifo_config: Read-write attribute to show and store FIFO configuration.
 * - self_test: Write-only attribute to store self-test values.
 * - iio_generic_buffer: Write-only attribute to store generic buffer values.
 */
static IIO_DEVICE_ATTR(chip_id, 0444, chip_id_show, NULL, 0);
static IIO_DEVICE_ATTR(reg_val, 0644, reg_val_show, reg_val_store, 0);
static IIO_DEVICE_ATTR(reg_sel, 0644, reg_sel_show, reg_sel_store, 0);
static IIO_DEVICE_ATTR(config_function, 0644, config_function_show,
						config_function_store, 0);
static IIO_DEVICE_ATTR(sensor_data, 0444, sensor_data_show, NULL, 0);
static IIO_DEVICE_ATTR(soft_reset, 0200, NULL, soft_reset_store, 0);
static IIO_DEVICE_ATTR(sensor_conf, 0644, sensor_conf_show,
						sensor_conf_store, 0);
static IIO_DEVICE_ATTR(driver_version, 0444,
							driver_version_show, NULL, 0);
static IIO_DEVICE_ATTR(avail_sensor, 0444, avail_sensor_show, NULL, 0);
static IIO_DEVICE_ATTR(op_mode, 0644, op_mode_show, op_mode_store, 0);
static IIO_DEVICE_ATTR(odr, 0644, odr_show, odr_store, 0);
static IIO_DEVICE_ATTR(fifo_data, 0444, fifo_data_show, NULL, 0);
static IIO_DEVICE_ATTR(fifo_config, 0644, fifo_config_show,
						fifo_config_store, 0);
static IIO_DEVICE_ATTR(self_test, 0200, NULL, self_test_store, 0);
static IIO_DEVICE_ATTR(iio_generic_buffer, 0200, NULL,
							iio_generic_buffer_store, 0);

/**
 * bmp3_attributes - Array of pointers to device attributes
 *
 * This array contains pointers to various device attributes for the BMP390
 * sensor driver. Each attribute represents a specific configuration or data
 * point that can be accessed or modified. The attributes include:
 *
 * - chip_id: Identifier for the chip
 * - reg_val: Value of a specific register
 * - reg_sel: Selected register
 * - config_function: Configuration function for the sensor
 * - sensor_data: Data read from the sensor
 * - soft_reset: Soft reset control
 * - sensor_conf: Sensor configuration
 * - driver_version: Version of the driver
 * - avail_sensor: Available sensors
 * - op_mode: Operating mode of the sensor
 * - odr: Output data rate
 * - fifo_data: Data from the FIFO buffer
 * - fifo_config: Configuration of the FIFO buffer
 * - self_test: Self-test control
 * - iio_generic_buffer: Generic buffer for IIO
 *
 * The array is terminated with a NULL pointer.
 */
static struct attribute *bmp3_attributes[] = {
	&iio_dev_attr_chip_id.dev_attr.attr,
	&iio_dev_attr_reg_val.dev_attr.attr,
	&iio_dev_attr_reg_sel.dev_attr.attr,
	&iio_dev_attr_config_function.dev_attr.attr,
	&iio_dev_attr_sensor_data.dev_attr.attr,
	&iio_dev_attr_soft_reset.dev_attr.attr,
	&iio_dev_attr_sensor_conf.dev_attr.attr,
	&iio_dev_attr_driver_version.dev_attr.attr,
	&iio_dev_attr_avail_sensor.dev_attr.attr,
	&iio_dev_attr_op_mode.dev_attr.attr,
	&iio_dev_attr_odr.dev_attr.attr,
	&iio_dev_attr_fifo_data.dev_attr.attr,
	&iio_dev_attr_fifo_config.dev_attr.attr,
	&iio_dev_attr_self_test.dev_attr.attr,
	&iio_dev_attr_iio_generic_buffer.dev_attr.attr,
	NULL
};

/**
 * struct attribute_group bmp3_attribute_group - Defines a group
 * of attributes for the BMP3 sensor.
 * @attrs: Pointer to the array of attribute structures.
 */
static struct attribute_group bmp3_attribute_group = {
	.attrs = bmp3_attributes
};

/**
 * struct iio_info bmp390_acc_iio_info - Provides information
 * and operations for the BMP390 accelerometer.
 * @attrs: Pointer to the attribute group for the BMP390.
 * @read_raw: Function pointer to read raw data from the BMP390 sensor.
 */
static const struct iio_info bmp390_acc_iio_info = {
	.attrs = &bmp3_attribute_group,
	/*lint -e546*/
	.read_raw = &bmp390_read_raw,
	/*lint +e546*/
};

#if defined(BMP3_ENABLE_INT1) || defined(BMP3_ENABLE_INT2)

/**
 * bmp3_irq_work_func - Work function to handle BMP3 interrupts
 * @work: Pointer to the work_struct associated with the interrupt
 *
 * This function is triggered by an interrupt and handles different types of
 * interrupts from the BMP3 sensor. It checks the interrupt status and performs
 * the appropriate actions based on the type of interrupt:
 * - FIFO Watermark Interrupt: Logs the occurrence and processes FIFO data.
 * - FIFO Full Interrupt: Logs the occurrence and processes FIFO data.
 * - Data Ready Interrupt: Logs the occurrence, disables
 * the data ready interrupt, and updates the sensor settings.
 *
 * The function uses the bmp3_get_status function to
 * retrieve the interrupt status
 * and the check_error function to handle any
 * errors that occur during processing.
 */
static void bmp3_irq_work_func(struct work_struct *work)
{
	/*lint -e26 -e10 -e124 -e40 -e831 -e64 -e119 -e413 -e534*/
	struct bmp3_client_data *client_data = container_of(work,
		struct bmp3_client_data, irq_work);
	/*lint +e26  +e10 +e124 +e40 +e831 +e64 +e119 +e413 +e534*/
	int ret = 0;
	struct bmp3_status status = { { 0 } };

	ret = bmp3_get_status(&status, &client_data->device);
	check_error("get int status", ret);

	if (ret == BMP3_OK) {
		if (status.intr.fifo_wm == BMP3_ENABLE &&
		    client_data->fifo_wm_en == BMP3_ENABLE) {
			pr_info("FIFO Watermark Interrupt occurred\n");
			ret = fifo_data_frame_print(client_data);
			check_error("fifo_data_frame_print", ret);
		} else if ((status.intr.fifo_full == BMP3_ENABLE) &&
				(client_data->fifo_full_en == BMP3_ENABLE)) {
			pr_info("FIFO Full Interrupt occurred\n");
			ret = fifo_data_frame_print(client_data);
			check_error("fifo_data_frame_print", ret);
		} else if ((status.intr.drdy == BMP3_ENABLE) &&
			(client_data->data_ready_en == BMP3_ENABLE)) {
			pr_info("Data ready Interrupt occurred\n");
			client_data->settings.int_settings.drdy_en = BMP3_DISABLE;
			client_data->settings_sel = BMP3_SEL_DRDY_EN;
			client_data->data_ready_en = BMP3_DISABLE;
			ret = bmp3_set_sensor_settings(client_data->settings_sel,
						       &client_data->settings,
						       &client_data->device);
			check_error("bmp3_set_sensor_settings", ret);
		}
	}
}

/**
 * bmp3_irq_handle - Interrupt handler for BMP3 sensor.
 * @irq: The interrupt number.
 * @handle: Pointer to the client data structure.
 *
 * This function is called when an interrupt is triggered by the BMP3 sensor.
 * It schedules work to be done in the irq_work workqueue.
 *
 * Return: Always returns IRQ_HANDLED.
 */
static irqreturn_t bmp3_irq_handle(int irq, void *handle)
{
	struct bmp3_client_data *client_data = handle;
	/*lint -e534*/
	schedule_work(&client_data->irq_work);
	/*lint +e534*/

	return IRQ_HANDLED;
}

/**
 * bmp3_request_irq - Request an IRQ and initialize work for the BMP390 sensor.
 * @client_data: Pointer to the BMP3 client data structure.
 *
 * This function requests an interrupt line for
 * the BMP390 sensor and initializes
 * a work structure for handling the interrupt.
 * It logs the IRQ request and handles
 * any errors that occur during the request.
 *
 * Return: 0 on success, -EIO if the IRQ request fails.
 */
static int bmp3_request_irq(struct bmp3_client_data *client_data)
{
	int ret = 0;

	pr_info("Request IRQ : %d\n", client_data->IRQ);
	ret = request_irq(client_data->IRQ, bmp3_irq_handle,
			  IRQF_TRIGGER_RISING,
			  SENSOR_NAME, client_data);
	if (ret < 0) {
		pr_err("request_irq failed with err:%d\n", ret);
		return -EIO;
	}
	/* Lint  -e69 */
	INIT_WORK(&client_data->irq_work, bmp3_irq_work_func);
	/* Lint +e69 */

	return ret;
}
#endif

/**
 * bmp3_probe - Probe function for the BMP390 sensor driver.
 * @bmp390_iio_private: Pointer to the IIO device structure.
 *
 * This function initializes the BMP390 sensor and registers it with the IIO
 * subsystem. It sets up the necessary device parameters, configures the buffer,
 * and requests IRQs if needed. If any step fails, it performs cleanup and
 * returns an appropriate error code.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bmp3_probe(struct iio_dev *bmp390_iio_private)
{
	int ret = 0;
	struct bmp3_client_data *client_data;

	pr_info("BMP390 function entrance\n");

	client_data = iio_priv(bmp390_iio_private);

	if (!client_data) {
		pr_err("client_data is NULL\n");
		return -EINVAL;
	}

	client_data->device.delay_us = bmp3_delay_us;
	bmp390_iio_private->channels = bmp390_iio_channels;
	bmp390_iio_private->num_channels = ARRAY_SIZE(bmp390_iio_channels);
	bmp390_iio_private->info = &bmp390_acc_iio_info;
	bmp390_iio_private->modes = INDIO_DIRECT_MODE;

	/*lint -e86*/
	mutex_init(&client_data->lock);
	/*lint +e86*/
	ret = bmp390_iio_configure_buffer(bmp390_iio_private);
	if (ret) {
		pr_err("Failed to configure buffer for bmp390: error %d\n", ret);
		goto exit_err_clean;
	}
	ret = iio_device_register(bmp390_iio_private);
	if (ret) {
		pr_err("Failed to register iio device for bmp390: error %d\n", ret);
		goto exit_err_clean;
	}

	ret = bmp3_init(&client_data->device);
	bmp3_delay_us(MS_TO_US(10), &client_data->device.intf_ptr);
	check_error("bmp3_init", ret);
	if (ret) {
		pr_err("bmp390 initialization failed with error %d\n", ret);
		goto exit_err_clean;
	}
	/* Request irq and config*/
	#if defined(BMP3_ENABLE_INT1) || defined(BMP3_ENABLE_INT2)
	ret = bmp3_request_irq(client_data);
	if (ret < 0) {
		pr_err("Failed to request IRQ for bmp390\n");
		goto exit_err_clean;
	}
	#endif

	pr_info("Sensor %s probed successfully\n", SENSOR_NAME);

	return 0;

exit_err_clean:
	bmp390_iio_unconfigure_buffer(bmp390_iio_private);
	if (bmp390_iio_private)
		iio_device_unregister(bmp390_iio_private);
	pr_err("Error occurred during bmp390 probe\n");
	return ret;
}

/**
 * bmp3_probe - Exported symbol for the BMP3 probe function.
 *
 * This symbol is exported for use in other kernel modules. The bmp3_probe
 * function is responsible for initializing the BMP3 sensor device.
 */
/* Lint -save -e19 */
EXPORT_SYMBOL(bmp3_probe);
/* Lint -restore */

/**
 * bmp3_remove - Remove the BMP390 IIO device
 * @bmp390_iio_private: Pointer to the IIO device structure
 *
 * This function performs the necessary cleanup when removing the BMP390
 * IIO device. It retrieves the client data, introduces a delay, unconfigures
 * the buffer, unregisters the device, and frees the IRQ.
 *
 * Return: Always returns 0.
 */
void bmp3_remove(struct iio_dev *bmp390_iio_private)
{
	struct bmp3_client_data *client_data;

	client_data = iio_priv(bmp390_iio_private);
	if (client_data) {
		bmp3_delay_us(MS_TO_US(300),
			      &client_data->device.intf_ptr);
		bmp390_iio_unconfigure_buffer(bmp390_iio_private);
		if (bmp390_iio_private)
			iio_device_unregister(bmp390_iio_private);
		(void)free_irq(client_data->IRQ, client_data);
	}
}
EXPORT_SYMBOL(bmp3_remove);

MODULE_AUTHOR("contact@bosch-sensortec.com");
MODULE_DESCRIPTION("BMP390 PRESSURE SENSOR DRIVER");
MODULE_LICENSE("GPL");

