// SPDX-License-Identifier: GPL-2.0
/*
 * Sensirion SLF3S liquid flow sensor driver.
 *
 * Supports the SLF3S-0600F, SLF3S-1300F and SLF3S-4000B liquid-flow
 * sensors over I2C.  Each measurement frame returns a 16-bit signed
 * flow value, a 16-bit signed temperature value and a status word,
 * each protected by a CRC-8 byte.
 *
 * The active calibration medium (water or isopropyl alcohol) is
 * runtime-switchable via the in_volumeflow_medium sysfs attribute and
 * defaults to water.
 *
 * Datasheet: https://sensirion.com/products/catalog/SLF3S-0600F/
 *
 * Copyright (C) 2026 CMBlu Energy GmbH
 * Author: Wadim Mueller <wafgo01@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/units.h>

#include <linux/iio/iio.h>

#define SLF3S_CRC8_POLY			0x31
#define SLF3S_CRC8_INIT			0xff

#define SLF3S_PRODUCT_ID_LEN		18
#define SLF3S_PRODUCT_FAMILY_BYTE	1
#define SLF3S_PRODUCT_SUBTYPE_BYTE	3
#define SLF3S_PRODUCT_FAMILY_ID		0x03

/* Datasheet section 2.2: tPU = 25 ms max from power-on to first cmd. */
#define SLF3S_POWER_UP_DELAY_US		(25 * USEC_PER_MSEC)
/* Datasheet section 2.2: tw = 60 ms typical until first valid sample. */
#define SLF3S_MEAS_START_DELAY_US	(60 * USEC_PER_MSEC)

static const u8 slf3s_cmd_prep_pid[]	= { 0x36, 0x7c };
static const u8 slf3s_cmd_read_pid[]	= { 0xe1, 0x02 };
static const u8 slf3s_cmd_start_water[]	= { 0x36, 0x08 };
static const u8 slf3s_cmd_start_ipa[]	= { 0x36, 0x15 };
static const u8 slf3s_cmd_stop_meas[]	= { 0x3f, 0xf9 };

enum slf3s_medium {
	SLF3S_MEDIUM_WATER,
	SLF3S_MEDIUM_IPA,
};

static const char * const slf3s_medium_modes[] = {
	[SLF3S_MEDIUM_WATER]	= "water",
	[SLF3S_MEDIUM_IPA]	= "ipa",
};

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
		.scale_den	= 600 * MICRO,
	},
	[1] = {
		.sub_type	= 0x02,
		.name		= "slf3s-1300f",
		.scale_num	= 1,
		.scale_den	= 30 * MICRO,
	},
	[2] = {
		.sub_type	= 0x05,
		.name		= "slf3s-4000b",
		.scale_num	= 1,
		.scale_den	= 1920 * MILLI,
	},
};

/**
 * struct slf3s_data - per-device state
 * @client:	I2C client this instance is bound to
 * @variant:	pointer into @slf3s_variants for the detected device
 * @medium:	currently active calibration medium
 * @crc_table:	pre-computed CRC-8 lookup table for SLF3S_CRC8_POLY
 */
struct slf3s_data {
	struct i2c_client *client;
	const struct slf3s_variant *variant;
	enum slf3s_medium medium;
	u8 crc_table[CRC8_TABLE_SIZE];
};

static bool slf3s_crc_valid(const struct slf3s_data *sf, const u8 *block)
{
	return crc8(sf->crc_table, block, 2, SLF3S_CRC8_INIT) == block[2];
}

static int slf3s_send_cmd(struct i2c_client *client, const u8 cmd[at_least 2])
{
	int ret = i2c_master_send(client, cmd, 2);

	if (ret == 2)
		return 0;

	return ret < 0 ? ret : -EIO;
}

/*
 * Read the product-info block and pick the matching variant.  The
 * sub-type byte returned by the sensor is the source of truth; a
 * DT-supplied compatible only seeds an initial guess and is overridden
 * on mismatch (with an informational message so misconfigured device
 * trees are easy to spot).
 *
 * Bus / CRC failures are real errors and fail probe.  An unknown
 * sub-type byte fails probe too: we cannot publish a meaningful scale
 * without a matching entry in slf3s_variants[].
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
		dev_info(&client->dev,
			 "unexpected family byte 0x%02x (expected 0x%02x)\n",
			 buf[SLF3S_PRODUCT_FAMILY_BYTE],
			 SLF3S_PRODUCT_FAMILY_ID);

	for (unsigned int i = 0; i < ARRAY_SIZE(slf3s_variants); i++) {
		if (buf[SLF3S_PRODUCT_SUBTYPE_BYTE] !=
		    slf3s_variants[i].sub_type)
			continue;

		if (sf->variant && sf->variant != &slf3s_variants[i])
			dev_info(&client->dev,
				 "DT compatible says %s but sensor reports %s; using %s\n",
				 sf->variant->name,
				 slf3s_variants[i].name,
				 slf3s_variants[i].name);

		sf->variant = &slf3s_variants[i];

		return 0;
	}

	dev_err(&client->dev, "unknown SLF3S sub-type 0x%02x\n",
		buf[SLF3S_PRODUCT_SUBTYPE_BYTE]);

	return -ENODEV;
}

static int slf3s_read_sample(struct slf3s_data *sf, int *flow, int *temp)
{
	u8 buf[9];
	int ret;

	ret = i2c_master_recv(sf->client, buf, ARRAY_SIZE(buf));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(buf))
		return -EIO;

	for (unsigned int i = 0; i < ARRAY_SIZE(buf); i += 3) {
		if (!slf3s_crc_valid(sf, &buf[i]))
			return -EIO;
	}

	*flow = sign_extend32(get_unaligned_be16(&buf[0]), 15);
	*temp = sign_extend32(get_unaligned_be16(&buf[3]), 15);

	return 0;
}

static int slf3s_get_medium(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan)
{
	struct slf3s_data *sf = iio_priv(indio_dev);

	return sf->medium;
}

static int slf3s_set_medium(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan, unsigned int mode)
{
	struct slf3s_data *sf = iio_priv(indio_dev);
	const u8 *start_cmd;
	int ret;

	if (!iio_device_claim_direct(indio_dev))
		return -EBUSY;

	ret = slf3s_send_cmd(sf->client, slf3s_cmd_stop_meas);
	if (ret)
		goto out;

	start_cmd = (mode == SLF3S_MEDIUM_IPA) ? slf3s_cmd_start_ipa
					       : slf3s_cmd_start_water;

	ret = slf3s_send_cmd(sf->client, start_cmd);
	if (ret)
		goto out;

	fsleep(SLF3S_MEAS_START_DELAY_US);
	sf->medium = mode;
out:
	iio_device_release_direct(indio_dev);

	return ret;
}

static const struct iio_enum slf3s_medium_enum = {
	.items		= slf3s_medium_modes,
	.num_items	= ARRAY_SIZE(slf3s_medium_modes),
	.get		= slf3s_get_medium,
	.set		= slf3s_set_medium,
};

static const struct iio_chan_spec_ext_info slf3s_ext_info[] = {
	IIO_ENUM("medium", IIO_SHARED_BY_TYPE, &slf3s_medium_enum),
	IIO_ENUM_AVAILABLE("medium", IIO_SHARED_BY_TYPE, &slf3s_medium_enum),
	{ }
};

static const struct iio_chan_spec slf3s_channels[] = {
	{
		.type = IIO_VOLUMEFLOW,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.ext_info = slf3s_ext_info,
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
		/* Temperature LSB = 1/200 degC; IIO_TEMP wants milli-degC. */
		*val = 1000 / 200;

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
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*sf));
	if (!indio_dev)
		return -ENOMEM;

	sf = iio_priv(indio_dev);
	sf->client = client;
	sf->variant = i2c_get_match_data(client);
	sf->medium = SLF3S_MEDIUM_WATER;
	crc8_populate_msb(sf->crc_table, SLF3S_CRC8_POLY);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd supply\n");

	fsleep(SLF3S_POWER_UP_DELAY_US);

	ret = slf3s_detect_variant(sf);
	if (ret)
		return dev_err_probe(dev, ret, "product info read failed\n");

	ret = slf3s_send_cmd(client, slf3s_cmd_start_water);
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
	{ .name = "slf3s-0600f",
	  .driver_data = (kernel_ulong_t)&slf3s_variants[0] },
	{ .name = "slf3s-1300f",
	  .driver_data = (kernel_ulong_t)&slf3s_variants[1] },
	{ .name = "slf3s-4000b",
	  .driver_data = (kernel_ulong_t)&slf3s_variants[2] },
	{ }
};
MODULE_DEVICE_TABLE(i2c, slf3s_id);

static const struct of_device_id slf3s_of_match[] = {
	{ .compatible = "sensirion,slf3s-0600f", .data = &slf3s_variants[0] },
	{ .compatible = "sensirion,slf3s-1300f", .data = &slf3s_variants[1] },
	{ .compatible = "sensirion,slf3s-4000b", .data = &slf3s_variants[2] },
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
