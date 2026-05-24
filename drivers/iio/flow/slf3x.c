// SPDX-License-Identifier: GPL-2.0
/*
 * Sensirion SLF3x liquid flow sensor driver.
 *
 * Copyright (C) 2026 CMBlu Energy
 * Author: Wadim Mueller <wadim.mueller@cmblu.de>
 */

#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <linux/unaligned.h>

#define SLF3X_CRC8_POLY		0x31
#define SLF3X_CRC8_INIT		0xff

#define SLF3X_PRODUCT_ID_LEN	18
#define SLF3X_FAMILY_BYTE	1
#define SLF3X_SUBTYPE_BYTE	3

#define SLF3X_FAMILY_ID		0x03

#define SLF3X_MEAS_LEN		9
#define SLF3X_MEAS_DELAY_US	12000

/* Temperature LSB is 1/200 °C; IIO_TEMP scale is in mC/LSB => 5. */
#define SLF3X_TEMP_SCALE	5

static const u8 slf3x_cmd_prep_pid[]	= { 0x36, 0x7c };
static const u8 slf3x_cmd_read_pid[]	= { 0xe1, 0x02 };
static const u8 slf3x_cmd_start_water[]	= { 0x36, 0x08 };
static const u8 slf3x_cmd_stop[]	= { 0x3f, 0xf9 };

DECLARE_CRC8_TABLE(slf3x_crc_table);

struct slf3x_variant {
	u8 sub_type;
	const char *name;
	/*
	 * Flow scale exposed via IIO_CHAN_INFO_SCALE in litres per second
	 * per LSB, encoded as IIO_VAL_FRACTIONAL (num / den).  The encoding
	 * comes from the Sensirion datasheet's "scale factor" (ticks per
	 * ml/min) combined with the 1 ml/min = 1/60000 l/s conversion.
	 */
	int scale_num;
	int scale_den;
};

static const struct slf3x_variant slf3x_variants[] = {
	{ .sub_type = 0x03, .name = "slf3s-0600f",
	  .scale_num = 1, .scale_den = 6000000 },
	{ .sub_type = 0x05, .name = "slf3s-4000b",
	  .scale_num = 1, .scale_den = 1666680000 },
};

struct slf3x_data {
	struct i2c_client *client;
	const struct slf3x_variant *variant;
};

static int slf3x_verify_crc(const u8 *block)
{
	return crc8(slf3x_crc_table, block, 2, SLF3X_CRC8_INIT) == block[2] ?
		       0 :
		       -EIO;
}

static int slf3x_write_cmd(struct i2c_client *client, const u8 *cmd)
{
	int ret = i2c_master_send(client, cmd, 2);

	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static int slf3x_read_product_info(struct slf3x_data *sf)
{
	struct i2c_client *client = sf->client;
	u8 buf[SLF3X_PRODUCT_ID_LEN];
	int ret, i;

	ret = slf3x_write_cmd(client, slf3x_cmd_prep_pid);
	if (ret)
		return ret;

	ret = slf3x_write_cmd(client, slf3x_cmd_read_pid);
	if (ret)
		return ret;

	ret = i2c_master_recv(client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	for (i = 0; i < SLF3X_PRODUCT_ID_LEN; i += 3) {
		if (slf3x_verify_crc(&buf[i])) {
			dev_err(&client->dev,
				"product-info CRC mismatch at byte %d\n", i);
			return -EIO;
		}
	}

	if (buf[SLF3X_FAMILY_BYTE] != SLF3X_FAMILY_ID) {
		dev_err(&client->dev,
			"unexpected device family 0x%02x\n",
			buf[SLF3X_FAMILY_BYTE]);
		return -ENODEV;
	}

	for (i = 0; i < ARRAY_SIZE(slf3x_variants); i++) {
		if (buf[SLF3X_SUBTYPE_BYTE] == slf3x_variants[i].sub_type) {
			sf->variant = &slf3x_variants[i];
			return 0;
		}
	}

	dev_err(&client->dev, "unsupported SLF3x sub-type 0x%02x\n",
		buf[SLF3X_SUBTYPE_BYTE]);
	return -ENODEV;
}

static int slf3x_read_sample(struct slf3x_data *sf, s16 *flow, s16 *temp)
{
	u8 buf[SLF3X_MEAS_LEN];
	int ret, i;

	ret = i2c_master_recv(sf->client, buf, sizeof(buf));
	if (ret != sizeof(buf))
		return ret < 0 ? ret : -EIO;

	for (i = 0; i < SLF3X_MEAS_LEN; i += 3) {
		if (slf3x_verify_crc(&buf[i]))
			return -EIO;
	}

	*flow = (s16)get_unaligned_be16(&buf[0]);
	*temp = (s16)get_unaligned_be16(&buf[3]);
	return 0;
}

static const struct iio_chan_spec slf3x_channels[] = {
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

static int slf3x_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan, int *val,
			  int *val2, long mask)
{
	struct slf3x_data *sf = iio_priv(indio_dev);
	s16 flow, temp;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = slf3x_read_sample(sf, &flow, &temp);
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
		*val = SLF3X_TEMP_SCALE;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info slf3x_info = {
	.read_raw = slf3x_read_raw,
};

static void slf3x_stop_meas(void *data)
{
	struct slf3x_data *sf = data;

	slf3x_write_cmd(sf->client, slf3x_cmd_stop);
}

static int slf3x_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct slf3x_data *sf;
	int ret;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret < 0 && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*sf));
	if (!indio_dev)
		return -ENOMEM;

	sf = iio_priv(indio_dev);
	sf->client = client;
	crc8_populate_msb(slf3x_crc_table, SLF3X_CRC8_POLY);

	ret = slf3x_read_product_info(sf);
	if (ret)
		return dev_err_probe(dev, ret, "product info read failed\n");

	ret = slf3x_write_cmd(client, slf3x_cmd_start_water);
	if (ret)
		return dev_err_probe(dev, ret, "start measurement failed\n");

	usleep_range(SLF3X_MEAS_DELAY_US, SLF3X_MEAS_DELAY_US + 1000);

	ret = devm_add_action_or_reset(dev, slf3x_stop_meas, sf);
	if (ret)
		return ret;

	indio_dev->name = sf->variant->name;
	indio_dev->channels = slf3x_channels;
	indio_dev->num_channels = ARRAY_SIZE(slf3x_channels);
	indio_dev->info = &slf3x_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id slf3x_id[] = {
	{ "slf3s" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, slf3x_id);

static const struct of_device_id slf3x_of_match[] = {
	{ .compatible = "sensirion,slf3s" },
	{ }
};
MODULE_DEVICE_TABLE(of, slf3x_of_match);

static struct i2c_driver slf3x_driver = {
	.driver = {
		.name = "slf3x",
		.of_match_table = slf3x_of_match,
	},
	.probe = slf3x_probe,
	.id_table = slf3x_id,
};
module_i2c_driver(slf3x_driver);

MODULE_AUTHOR("Wadim Mueller <wadim.mueller@cmblu.de>");
MODULE_DESCRIPTION("Sensirion SLF3x liquid flow sensor driver");
MODULE_LICENSE("GPL");
