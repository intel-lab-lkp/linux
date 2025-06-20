// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp390_i2c.c
 * @date		2025-06-02
 * @version		v2.2.0
 *
 * @brief		 BMP390 I2C bus Driver
 *
 */

/*********************************************************************/
/* system header files */
/*********************************************************************/
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/input.h>

/*********************************************************************/
/* own header files */
/*********************************************************************/
#include "bmp390_driver.h"

/*********************************************************************/
/* global variables */
/*********************************************************************/
static struct i2c_client *bmp3_i2c_client;
struct iio_dev *iio_i2c_dev;

/**
 *	bmp390_i2c_init - I2C driver init function.
 *
 *	Return : Status of the suspend function.
 *	* 0 - OK.
 *	* Negative value - Error.
 */
static int __init bmp390_i2c_init(void);

/**
 *	bmp390_i2c_exit - I2C driver exit function.
 */
static void __exit bmp390_i2c_exit(void);

/**
 * bmp390_i2c_read - The I2C read function.
 *
 * @client : Instance of the I2C client
 * @reg_addr : The register address from where the data is read.
 * @sdata : The pointer to buffer to return data.
 * @len : The number of bytes to be read
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp390_i2c_read(struct i2c_client *client,
			  u8 reg_addr, u8 *sdata, u16 len)
{
	s32 retry;
	struct i2c_msg msg[] = {
		{
		.addr = client->addr,
		.flags = 0,
		.len = 1,
		.buf = &reg_addr,
		},

		{
		.addr = client->addr,
		.flags = I2C_M_RD,
		.len = len,
		.buf = sdata,
		},
	};

	for (retry = 0; retry < BMP3_MAX_RETRY_I2C_XFER; retry++) {
		if (i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg)) > 0)
			break;
		usleep_range(BMP3_I2C_WRITE_DELAY_TIME * 1000,
			     BMP3_I2C_WRITE_DELAY_TIME * 1000);
	}

	if (retry >= BMP3_MAX_RETRY_I2C_XFER) {
		pr_err("I2C xfer error\n");
		return -EIO;
	}

	return 0;
}

/**
 * bmp390_i2c_write - The I2C write function.
 *
 * @client : Instance of the I2C client
 * @reg_addr : The register address to start writing the data.
 * @sdata : The pointer to buffer holding data to be written.
 * @len : The number of bytes to write.
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp390_i2c_write(struct i2c_client *client,
			   u8 reg_addr, const u8 *sdata, u16 len)
{
	s32 retry;

	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = len + 1,
		.buf = NULL,
	};
	msg.buf = kmalloc(len + 1, GFP_KERNEL);
	if (!msg.buf) {
		pr_err("Allocate memory failed\n");
		return -ENOMEM;
	}
	msg.buf[0] = reg_addr;
	memcpy(&msg.buf[1], sdata, len);
	for (retry = 0; retry < BMP3_MAX_RETRY_I2C_XFER; retry++) {
		if (i2c_transfer(client->adapter, &msg, 1) > 0)
			break;
		usleep_range(BMP3_I2C_WRITE_DELAY_TIME * 1000,
			     BMP3_I2C_WRITE_DELAY_TIME * 1000);
	}
	kfree(msg.buf);
	if (retry >= BMP3_MAX_RETRY_I2C_XFER) {
		pr_err("I2C xfer error\n");
		return -EIO;
	}

	return 0;
}

/**
 * bmp390_i2c_read_wrapper -
 * The I2C read function pointer used by BMP390 API.
 *
 * @dev_addr : I2c Device address
 * @reg_addr : The register address to read the data.
 * @sdata : The pointer to buffer to return data.
 * @len : The number of bytes to be read
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp390_i2c_read_wrapper(u8 reg_addr, u8 *sdata,
				  u32 len, void *intf_ptr)
{
	s8 err;

	err = bmp390_i2c_read(bmp3_i2c_client, reg_addr, sdata, len);
	return err;
}

/**
 * bmp390_i2c_write_wrapper - The I2C write function pointer used by BMP390 API.
 *
 * @dev_addr : I2c Device address
 * @reg_addr : The register address to start writing the data.
 * @sdata : The pointer to buffer which holds the data to be written.
 * @len : The number of bytes to be written.
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp390_i2c_write_wrapper(u8 reg_addr, const u8 *sdata,
				   u32 len, void *intf_ptr)
{
	s8 err;

	err = bmp390_i2c_write(bmp3_i2c_client, reg_addr, sdata, len);
	return err;
}

/**
 * bmp390_i2c_probe - The I2C probe function called by I2C bus driver.
 *
 * @client : The I2C client instance
 * @id : The I2C device ID instance
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static int bmp390_i2c_probe(struct i2c_client *client)
{
	int err;
	u8 dev_id;
	const struct i2c_device_id *id;
	struct bmp3_client_data *client_data = NULL;

	pr_info("entrance\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("i2c_check_functionality error!\n");
		pr_err("I2C adapter is not supported\n");
		err = -EIO;
		goto exit_err_clean;
	}
	id = i2c_client_get_device_id(client);
	if (!bmp3_i2c_client) {
		bmp3_i2c_client = client;
	} else {
		pr_err("this driver does not support multiple clients\n");
		err = -EBUSY;
		goto exit_err_clean;
	}

	client_data = kzalloc(sizeof(*client_data),
			      GFP_KERNEL);
	if (!client_data) {
		err = -ENOMEM;
		goto exit_err_clean;
	}
	iio_i2c_dev = devm_iio_device_alloc(&client->dev,
					    sizeof(*client_data));
	if (!iio_i2c_dev)
		return -ENOMEM;
	/* h/w init */

	client_data->device.intf_ptr = client;
	dev_id = BMP3_I2C_INTF;
	client_data = iio_priv(iio_i2c_dev);
	client_data->device.intf_ptr = &dev_id;
	client_data->device.intf = BMP3_I2C_INTF;
	client_data->device.read = bmp390_i2c_read_wrapper;
	client_data->device.write = bmp390_i2c_write_wrapper;
	client_data->IRQ = client->irq;
	iio_i2c_dev->dev.parent = &client->dev;
	iio_i2c_dev->name = "bmp390";
	iio_i2c_dev->modes = INDIO_DIRECT_MODE;
	/*only assign iio_i2c_dev*/
	i2c_set_clientdata(client, iio_i2c_dev);
	if (id)
		client_data->name = id->name;

	pr_info("call BMP390 probe\n");
	return bmp3_probe(iio_i2c_dev);

exit_err_clean:
	if (err)
		bmp3_i2c_client = NULL;
	i2c_set_clientdata(client, NULL);
	if (iio_i2c_dev)
		iio_device_free(iio_i2c_dev);
	return err;
}

/**
 *	bmp390_i2c_remove - Callback called when device is unbinded.
 *	@client : Instance of I2C client device.
 *
 *	Return : Status of the suspend function.
 *	* 0 - OK.
 *	* Negative value - Error.
 */
static void bmp390_i2c_remove(struct i2c_client *client)
{
	bmp3_remove(iio_i2c_dev);
	bmp3_i2c_client = NULL;
}

static const struct i2c_device_id bmp390_id[] = {
	{ SENSOR_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bmp390_id);

static const struct of_device_id bmp390_of_match[] = {
	{ .compatible = "bosch,bmp390", },
	{ }
};
MODULE_DEVICE_TABLE(of, bmp390_of_match);

static struct i2c_driver bmp390_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = SENSOR_NAME,
		.of_match_table = bmp390_of_match,
	},
	.class = I2C_CLASS_HWMON,
	.id_table = bmp390_id,
	.probe = bmp390_i2c_probe,
	.remove = bmp390_i2c_remove,
};

/**
 *	bmp390_i2c_init - I2C driver init function.
 *
 *	Return : Status of the suspend function.
 *	* 0 - OK.
 *	* Negative value - Error.
 */
static int __init bmp390_i2c_init(void)
{
	return i2c_add_driver(&bmp390_driver);
}

/**
 *	bmp390_i2c_exit - I2C driver exit function.
 */
static void __exit bmp390_i2c_exit(void)
{
	i2c_del_driver(&bmp390_driver);
}

MODULE_AUTHOR("contact@bosch-sensortec.com>");
MODULE_DESCRIPTION("BMP390 SENSOR I2C DRIVER");
MODULE_LICENSE("GPL");
/*lint -e19 -e546 -e611*/
module_init(bmp390_i2c_init);
module_exit(bmp390_i2c_exit);
/*lint +e19 +e546 +e611*/

