// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/**
 * @section LICENSE
 * Copyright (c) 2024 Bosch Sensortec GmbH All Rights Reserved.
 *
 * This software program is licensed subject to the GNU General
 * Public License (GPL).Version 2,June 1991,
 * available at http://www.fsf.org/copyleft/gpl.html
 *
 * @file		bmp390_spi.c
 * @date		2025-06-02
 * @version		v2.2.0
 *
 * @brief		BMP3 SPI bus Driver
 *
 */

/*********************************************************************/
/* system header files */
/*********************************************************************/
#include <linux/module.h>
#include <linux/spi/spi.h>
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
/* Local macro definitions */
/*********************************************************************/
#define BMP3_MAX_BUFFER_SIZE		32

/*********************************************************************/
/* global variables */
/*********************************************************************/
static struct spi_device *bmp3_spi_client;
struct iio_dev *iio_spi_dev;

/*!
 * @brief define spi block write function
 *
 * @param[in] reg_addr register address
 * @param[in] sdata the pointer of data buffer
 * @param[in] len block size need to write
 *
 * @return zero success, non-zero failed
 * @retval zero success
 * @retval non-zero failed
 */
static s8 bmp3_spi_write_block(u8 reg_addr, const u8 *sdata, u8 len)
{
	struct spi_device *client = bmp3_spi_client;
	u8 buffer[BMP3_MAX_BUFFER_SIZE + 1];
	struct spi_transfer xfer = {
		.tx_buf = buffer,
		.len = len + 1,
	};
	struct spi_message msg;

	if (len > BMP3_MAX_BUFFER_SIZE)
		return -EINVAL;

	buffer[0] = reg_addr & 0x7F;/* write: MSB = 0 */
	memcpy(&buffer[1], sdata, len);

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	return spi_sync(client, &msg);
}

/*!
 * @brief define spi block read function
 *
 * @param[in] reg_addr register address
 * @param[out] sdata the pointer of data buffer
 * @param[in] len block size need to read
 *
 * @return zero success, non-zero failed
 * @retval zero success
 * @retval non-zero failed
 */
static s8 bmp3_spi_read_block(u8 reg_addr, u8 *sdata, uint16_t len)
{
	struct spi_device *client = bmp3_spi_client;
	u8 reg = reg_addr | 0x80;/* read: MSB = 1 */
	struct spi_transfer xfer[2] = {
		[0] = {
			.tx_buf = &reg,
			.len = 1,
		},
		[1] = {
			.rx_buf = sdata,
			.len = len,
		}
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	spi_message_add_tail(&xfer[1], &msg);
	return spi_sync(client, &msg);
}

/**
 * bmp3_spi_write_wrapper - The SPI write function pointer used by BMP3 API.
 *
 * @reg_addr : The register address to start writing the data.
 * @sdata : The pointer to buffer which holds the data to be written.
 * @len : The number of bytes to be written.
 * @intf_ptr  : Void pointer that can enable the linking of descriptors
 *			for interface related call backs.
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp3_spi_write_wrapper(u8 reg_addr, const u8 *sdata,
				 u32 len, void *intf_ptr)
{
	s8 err;

	err = bmp3_spi_write_block(reg_addr, sdata, len);
	return err;
}

/**
 * bmp3_spi_read_wrapper - The SPI read function pointer used by BMP3 API.
 *
 * @reg_addr : The register address to read the data.
 * @sdata : The pointer to buffer to return data.
 * @len : The number of bytes to be read
 *
 * Return : Status of the function.
 * * 0 - OK
 * * negative value - Error.
 */
static s8 bmp3_spi_read_wrapper(u8 reg_addr,
				u8 *sdata, u32 len, void *intf_ptr)
{
	s8 err;

	err = bmp3_spi_read_block(reg_addr, sdata, len);
	return err;
}

/*!
 * @brief sensor probe function via spi bus
 *
 * @param[in] client the pointer of spi client
 *
 * @return zero success, non-zero failed
 * @retval zero success
 * @retval non-zero failed
 */
static int bmp3_spi_probe(struct spi_device *client)
{
	int status;
	int err = 0;
	u8 dev_id;
	struct bmp3_client_data *client_data = NULL;

	if (!bmp3_spi_client) {
		bmp3_spi_client = client;
	} else {
		pr_err("This driver does not support multiple clients!\n");
		return -EBUSY;
	}
	client->bits_per_word = 8;
	status = spi_setup(client);
	if (status < 0) {
		pr_err("spi_setup failed!\n");
		return status;
	}
	client_data = kzalloc(sizeof(*client_data), GFP_KERNEL);
	if (!client_data) {
		err = -ENOMEM;
		goto exit_err_clean;
	}
	iio_spi_dev = devm_iio_device_alloc(&client->dev, sizeof(*client_data));
	if (!iio_spi_dev)
		return -ENOMEM;
	client_data = iio_priv(iio_spi_dev);
	client_data->dev = &client->dev;
	dev_id = BMP3_SPI_INTF;
	client_data->device.intf_ptr = &dev_id;
	client_data->device.intf = BMP3_SPI_INTF;
	client_data->IRQ = client->irq;
	client_data->device.read = bmp3_spi_read_wrapper;
	client_data->device.write = bmp3_spi_write_wrapper;
	iio_spi_dev->dev.parent = &client->dev;
	iio_spi_dev->name = "bmp390";
	iio_spi_dev->modes = INDIO_DIRECT_MODE;
	dev_set_drvdata(&client->dev, iio_spi_dev);

	return bmp3_probe(iio_spi_dev);

exit_err_clean:
	if (err)
		bmp3_spi_client = NULL;
	if (iio_spi_dev)
		iio_device_free(iio_spi_dev);
	return err;
}

/*!
 * @brief remove bmi spi client
 *
 * @param[in] client the pointer of spi client
 *
 * @return zero
 * @retval zero
 */
static void bmp3_spi_remove(struct spi_device *client)
{
	bmp3_remove(iio_spi_dev);
	bmp3_spi_client = NULL;
}

/*!
 * @brief register spi device id
 */
static const struct spi_device_id bmp3_id[] = {
	{ SENSOR_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, bmp3_id);

/*!
 * @brief register bmp3 device id match
 */
static const struct of_device_id bmp3_of_match[] = {
	{ .compatible = "bosch,bmp390", },
	{ }
};
MODULE_DEVICE_TABLE(of, bmp3_of_match);

/*!
 * @brief register spi driver hooks
 */
static struct spi_driver bmp3_spi_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = SENSOR_NAME,
		.of_match_table = bmp3_of_match,
	},
	.id_table = bmp3_id,
	.probe	  = bmp3_spi_probe,
	.remove	  = bmp3_spi_remove,
};

/*!
 * @brief initialize bmi spi module
 *
 * @return zero success, non-zero failed
 * @retval zero success
 * @retval non-zero failed
 */
static int __init bmp3_spi_init(void)
{
	return spi_register_driver(&bmp3_spi_driver);
}

/*!
 * @brief remove bmi spi module
 *
 * @return no return value
 */
static void __exit bmp3_spi_exit(void)
{
	spi_unregister_driver(&bmp3_spi_driver);
}

MODULE_AUTHOR("Contact <contact@bosch-sensortec.com>");
MODULE_DESCRIPTION("BMP390 SENSOR SPI DRIVER");
MODULE_LICENSE("GPL");
/*lint -e19 -e546 -e611*/
module_init(bmp3_spi_init);
module_exit(bmp3_spi_exit);
/*lint +e19 +e546 +e611*/

