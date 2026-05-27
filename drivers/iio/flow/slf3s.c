// SPDX-License-Identifier: GPL-2.0
/*
 * Sensirion SLF3S liquid flow sensor driver.
 *
 * Supports the SLF3S-0600F, SLF3S-1300F and SLF3S-4000B liquid-flow
 * sensors over I2C.  Each measurement frame returns a 16-bit signed
 * flow value, a 16-bit signed temperature value and a status word,
 * each protected by a CRC-8 byte.
 *
 * Datasheet: https://sensirion.com/products/catalog/SLF3S-0600F/
 *
 * Copyright (C) 2026 CMBlu Energy GmbH
 * Author: Wadim Mueller <wafgo01@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>

#define SLF3S_CRC8_POLY			0x31
#define SLF3S_CRC8_INIT			0xff

#define SLF3S_PRODUCT_ID_LEN		18
#define SLF3S_PRODUCT_FAMILY_BYTE	1
#define SLF3S_PRODUCT_SUBTYPE_BYTE	3
#define SLF3S_PRODUCT_FAMILY_ID		0x03

#define SLF3S_MEAS_LEN			9
#define SLF3S_MEAS_START_DELAY_US	12000

/*
 * Temperature LSB equals 1/200 degC.  IIO_TEMP uses milli-degrees,
 * therefore the scale exposed to userspace is 1000 / 200 = 5.
 */
#define SLF3S_TEMP_SCALE_MILLIC		5

static const u8 slf3s_cmd_prep_pid[]	= { 0x36, 0x7c };
static const u8 slf3s_cmd_read_pid[]	= { 0xe1, 0x02 };
static const u8 slf3s_cmd_start_water[]	= { 0x36, 0x08 };
static const u8 slf3s_cmd_start_ipa[]	= { 0x36, 0x15 };
static const u8 slf3s_cmd_stop_meas[]	= { 0x3f, 0xf9 };

/**
 * struct slf3s_variant - per-variant calibration constants
 * @sub_type:	product-info sub-type byte returned by the sensor
 * @name:	name reported via @iio_dev.name
 * @scale_num:	flow scale numerator (l/s per LSB)
 * @scale_den:	flow scale denominator (l/s per LSB)
 */
struct slf3s_variant {
	u8 sub_type;
	const char *name;
	int scale_num;
	int scale_den;
};

static const struct slf3s_variant slf3s_variants[] = {
	[0] = {
		.sub_type	= 0x03,
		.name		= "slf3s-0600f",
		.scale_num	= 1,
		.scale_den	= 600000000,
	},
	[1] = {
		.sub_type	= 0x02,
		.name		= "slf3s-1300f",
		.scale_num	= 1,
		.scale_den	= 30000000,
	},
	[2] = {
		.sub_type	= 0x05,
		.name		= "slf3s-4000b",
		.scale_num	= 1,
		.scale_den	= 1920000,
	},
};

/**
 * struct slf3s_data - per-device state
 * @client:	I2C client this instance is bound to
 * @variant:	pointer into @slf3s_variants for the detected device
 * @crc_table:	pre-computed CRC-8 lookup table for SLF3S_CRC8_POLY
 */
struct slf3s_data {
	struct i2c_client *client;
	const struct slf3s_variant *variant;
	u8 crc_table[CRC8_TABLE_SIZE];
};

static bool slf3s_crc_valid(const struct slf3s_data *sf, const u8 *block)
{
	return crc8(sf->crc_table, block, 2, SLF3S_CRC8_INIT) == block[2];
}

static int slf3s_send_cmd(struct i2c_client *client, const u8 cmd[static 2])
{
	int ret = i2c_master_send(client, cmd, 2);

	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

/*
 * Read the product-info block and update @sf->variant.  The kernel
 * trusts the DT compatible (or i2c id_table .data) above all else; the
 * sub-type byte is a sanity hint.  This means:
 *
 *   - bus / CRC failures are real errors and must fail probe;
 *   - if the caller already picked a variant (specific compatible), the
 *     PID is logged for diagnostics but mismatches do not fail probe;
 *   - if the caller has no variant (generic "sensirion,slf3s" fallback),
 *     the sub-type byte is used to pick one; unknown sub-type fails.
 */
static int slf3s_detect_variant(struct slf3s_data *sf)
{
	struct i2c_client *client = sf->client;
	u8 buf[SLF3S_PRODUCT_ID_LEN];
	int ret;

	ret = slf3s_send_cmd(client, slf3s_cmd_prep_pid);
	if (ret)
		return ret;

	ret = slf3s_send_cmd(client, slf3s_cmd_read_pid);
	if (ret)
		return ret;

	ret = i2c_master_recv(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	for (unsigned int i = 0; i < SLF3S_PRODUCT_ID_LEN; i += 3) {
		if (!slf3s_crc_valid(sf, &buf[i]))
			return -EIO;
	}

	if (buf[SLF3S_PRODUCT_FAMILY_BYTE] != SLF3S_PRODUCT_FAMILY_ID)
		dev_dbg(&client->dev,
			"unexpected family byte 0x%02x (expected 0x%02x)\n",
			buf[SLF3S_PRODUCT_FAMILY_BYTE],
			SLF3S_PRODUCT_FAMILY_ID);

	for (unsigned int i = 0; i < ARRAY_SIZE(slf3s_variants); i++) {
		if (buf[SLF3S_PRODUCT_SUBTYPE_BYTE] !=
		    slf3s_variants[i].sub_type)
			continue;

		if (sf->variant && sf->variant != &slf3s_variants[i])
			dev_dbg(&client->dev,
				"DT compatible says %s but sub-type 0x%02x suggests %s\n",
				sf->variant->name,
				buf[SLF3S_PRODUCT_SUBTYPE_BYTE],
				slf3s_variants[i].name);
		else if (!sf->variant)
			sf->variant = &slf3s_variants[i];
		return 0;
	}

	if (sf->variant) {
		dev_dbg(&client->dev,
			"unknown SLF3S sub-type 0x%02x, trusting DT compatible %s\n",
			buf[SLF3S_PRODUCT_SUBTYPE_BYTE], sf->variant->name);
		return 0;
	}

	dev_dbg(&client->dev, "unknown SLF3S sub-type 0x%02x\n",
		buf[SLF3S_PRODUCT_SUBTYPE_BYTE]);
	return -ENODEV;
}

static int slf3s_read_sample(struct slf3s_data *sf, int *flow, int *temp)
{
	u8 buf[SLF3S_MEAS_LEN];
	int ret;

	ret = i2c_master_recv(sf->client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	for (unsigned int i = 0; i < SLF3S_MEAS_LEN; i += 3) {
		if (!slf3s_crc_valid(sf, &buf[i]))
			return -EIO;
	}

	*flow = sign_extend32(get_unaligned_be16(&buf[0]), 15);
	*temp = sign_extend32(get_unaligned_be16(&buf[3]), 15);
	return 0;
}

static const struct iio_chan_spec slf3s_channels[] = {
	{
		.type = IIO_VOLUMEFLOW,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int slf3s_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan, int *val,
			  int *val2, long mask)
{
	struct slf3s_data *sf = iio_priv(indio_dev);
	int flow, temp, ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		ret = slf3s_read_sample(sf, &flow, &temp);
		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;

		*val = (chan->type == IIO_VOLUMEFLOW) ? flow : temp;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_VOLUMEFLOW) {
			*val = sf->variant->scale_num;
			*val2 = sf->variant->scale_den;
			return IIO_VAL_FRACTIONAL;
		}
		*val = SLF3S_TEMP_SCALE_MILLIC;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info slf3s_info = {
	.read_raw = slf3s_read_raw,
};

static void slf3s_stop_meas(void *data)
{
	struct slf3s_data *sf = data;

	slf3s_send_cmd(sf->client, slf3s_cmd_stop_meas);
}

static int slf3s_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct slf3s_data *sf;
	const u8 *start_cmd = slf3s_cmd_start_water;
	const char *medium;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*sf));
	if (!indio_dev)
		return -ENOMEM;

	sf = iio_priv(indio_dev);
	sf->client = client;
	sf->variant = i2c_get_match_data(client);
	crc8_populate_msb(sf->crc_table, SLF3S_CRC8_POLY);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd supply\n");

	ret = slf3s_detect_variant(sf);
	if (ret)
		return dev_err_probe(dev, ret, "product info read failed\n");

	ret = device_property_read_string(dev, "sensirion,medium", &medium);
	if (!ret) {
		if (!strcmp(medium, "ipa"))
			start_cmd = slf3s_cmd_start_ipa;
		else if (strcmp(medium, "water"))
			return dev_err_probe(dev, -EINVAL,
					     "unknown sensirion,medium '%s'\n",
					     medium);
	}

	ret = slf3s_send_cmd(client, start_cmd);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to start measurement\n");

	fsleep(SLF3S_MEAS_START_DELAY_US);

	ret = devm_add_action_or_reset(dev, slf3s_stop_meas, sf);
	if (ret)
		return ret;

	indio_dev->name = sf->variant->name;
	indio_dev->channels = slf3s_channels;
	indio_dev->num_channels = ARRAY_SIZE(slf3s_channels);
	indio_dev->info = &slf3s_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id slf3s_id[] = {
	{ .name = "slf3s-0600f", .driver_data = (kernel_ulong_t)&slf3s_variants[0] },
	{ .name = "slf3s-1300f", .driver_data = (kernel_ulong_t)&slf3s_variants[1] },
	{ .name = "slf3s-4000b", .driver_data = (kernel_ulong_t)&slf3s_variants[2] },
	{ .name = "slf3s" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, slf3s_id);

static const struct of_device_id slf3s_of_match[] = {
	{ .compatible = "sensirion,slf3s-0600f", .data = &slf3s_variants[0] },
	{ .compatible = "sensirion,slf3s-1300f", .data = &slf3s_variants[1] },
	{ .compatible = "sensirion,slf3s-4000b", .data = &slf3s_variants[2] },
	{ .compatible = "sensirion,slf3s" },
	{ }
};
MODULE_DEVICE_TABLE(of, slf3s_of_match);

static struct i2c_driver slf3s_driver = {
	.driver = {
		.name		= "slf3s",
		.of_match_table	= slf3s_of_match,
	},
	.probe		= slf3s_probe,
	.id_table	= slf3s_id,
};
module_i2c_driver(slf3s_driver);

MODULE_AUTHOR("Wadim Mueller <wafgo01@gmail.com>");
MODULE_DESCRIPTION("Sensirion SLF3S liquid flow sensor driver");
MODULE_LICENSE("GPL");
