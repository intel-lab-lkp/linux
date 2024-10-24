// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 IIO driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/iio/iio.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-iio"

/* Host interface */
#define REQUEST_RPT_MOD		0xFF
#define REQUEST_IIO_MOD		0x00

/* Report Channel */
#define IIO_VIN_STS(x)		(0x68 + (x))
#define IIO_TMP_STS(x)		(0x6A + (x))
#define IIO_TMP_STS_CH(x)		\
	({ typeof(x) x_ = x;		\
	   (x_ < 10) ? x_ : x_ + 6; })

/* Message Channel*/
/* Command 00h */
#define REQUEST_IIO_CMD0_LEN	0x40
#define REQUEST_IIO_CMD0_OFFSET 0x0000	/* OFFSET = SEL|CMD */
#define IIO_VIN_EN(x)		(0x00 + (x))
#define IIO_TMP_EN(x)		(0x02 + (x))
/* Command 02h */
#define REQUEST_IIO_CMD2_LEN	0x90
#define REQUEST_IIO_CMD2_OFFSET 0x0002	/* OFFSET = SEL|CMD */
#define IIO_SMI_CTRL_IDX	0x00
#define IIO_VIN_LIMIT_IDX(x)	(0x10 + ((x) * 2))
#define IIO_TMP_LIMIT_IDX(x)	(0x30 + ((x) * 2))
#define IIO_CMD2_HYST_MASK	0x1F
/* Command 03h */
#define REQUEST_IIO_CMD3_LEN	0x08
#define REQUEST_IIO_CMD3_OFFSET 0x0003	/* OFFSET = SEL|CMD */

struct nct6694_iio_data {
	struct nct6694 *nct6694;

	/* Make sure read & write commands are consecutive */
	struct mutex iio_lock;
};

static const struct iio_event_spec nct6694_volt_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}
};

static const struct iio_event_spec nct6694_temp_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}
};

#define NCT6694_VOLTAGE_CHANNEL(num, addr) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = num,						\
	.address = addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_ENABLE) |	\
			      BIT(IIO_CHAN_INFO_PROCESSED),	\
	.event_spec = nct6694_volt_events,			\
	.num_event_specs = ARRAY_SIZE(nct6694_volt_events),	\
}

#define NCT6694_TEMPERATURE_CHANNEL(num, addr) {		\
	.type = IIO_TEMP,					\
	.indexed = 1,						\
	.channel = num,						\
	.address = addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_ENABLE) |	\
			      BIT(IIO_CHAN_INFO_PROCESSED) |	\
			      BIT(IIO_CHAN_INFO_HYSTERESIS),	\
	.event_spec = nct6694_temp_events,			\
	.num_event_specs = ARRAY_SIZE(nct6694_temp_events),	\
}

static const struct iio_chan_spec nct6694_iio_channels[] = {
	NCT6694_VOLTAGE_CHANNEL(0, 0x0),	/* VIN0 */
	NCT6694_VOLTAGE_CHANNEL(1, 0x1),	/* VIN1 */
	NCT6694_VOLTAGE_CHANNEL(2, 0x2),	/* VIN2 */
	NCT6694_VOLTAGE_CHANNEL(3, 0x3),	/* VIN3 */
	NCT6694_VOLTAGE_CHANNEL(4, 0x4),	/* VIN5 */
	NCT6694_VOLTAGE_CHANNEL(5, 0x5),	/* VIN6 */
	NCT6694_VOLTAGE_CHANNEL(6, 0x6),	/* VIN7 */
	NCT6694_VOLTAGE_CHANNEL(7, 0x7),	/* VIN14 */
	NCT6694_VOLTAGE_CHANNEL(8, 0x8),	/* VIN15 */
	NCT6694_VOLTAGE_CHANNEL(9, 0x9),	/* VIN16 */
	NCT6694_VOLTAGE_CHANNEL(10, 0xA),	/* VBAT */
	NCT6694_VOLTAGE_CHANNEL(11, 0xB),	/* VSB */
	NCT6694_VOLTAGE_CHANNEL(12, 0xC),	/* AVSB */
	NCT6694_VOLTAGE_CHANNEL(13, 0xD),	/* VCC */
	NCT6694_VOLTAGE_CHANNEL(14, 0xE),	/* VHIF */
	NCT6694_VOLTAGE_CHANNEL(15, 0xF),	/* VTT */

	NCT6694_TEMPERATURE_CHANNEL(0, 0x10),	/* THR1 */
	NCT6694_TEMPERATURE_CHANNEL(1, 0x12),	/* THR2 */
	NCT6694_TEMPERATURE_CHANNEL(2, 0x14),	/* THR14 */
	NCT6694_TEMPERATURE_CHANNEL(3, 0x16),	/* THR15 */
	NCT6694_TEMPERATURE_CHANNEL(4, 0x18),	/* THR16 */
	NCT6694_TEMPERATURE_CHANNEL(5, 0x1A),	/* TDP0 */
	NCT6694_TEMPERATURE_CHANNEL(6, 0x1C),	/* TDP1 */
	NCT6694_TEMPERATURE_CHANNEL(7, 0x1E),	/* TDP2 */
	NCT6694_TEMPERATURE_CHANNEL(8, 0x20),	/* TDP3 */
	NCT6694_TEMPERATURE_CHANNEL(9, 0x22),	/* TDP4 */

	NCT6694_TEMPERATURE_CHANNEL(10, 0x30),	/* DTIN0 */
	NCT6694_TEMPERATURE_CHANNEL(11, 0x32),	/* DTIN1 */
	NCT6694_TEMPERATURE_CHANNEL(12, 0x34),	/* DTIN2 */
	NCT6694_TEMPERATURE_CHANNEL(13, 0x36),	/* DTIN3 */
	NCT6694_TEMPERATURE_CHANNEL(14, 0x38),	/* DTIN4 */
	NCT6694_TEMPERATURE_CHANNEL(15, 0x3A),	/* DTIN5 */
	NCT6694_TEMPERATURE_CHANNEL(16, 0x3C),	/* DTIN6 */
	NCT6694_TEMPERATURE_CHANNEL(17, 0x3E),	/* DTIN7 */
	NCT6694_TEMPERATURE_CHANNEL(18, 0x40),	/* DTIN8 */
	NCT6694_TEMPERATURE_CHANNEL(19, 0x42),	/* DTIN9 */
	NCT6694_TEMPERATURE_CHANNEL(20, 0x44),	/* DTIN10 */
	NCT6694_TEMPERATURE_CHANNEL(21, 0x46),	/* DTIN11 */
	NCT6694_TEMPERATURE_CHANNEL(22, 0x48),	/* DTIN12 */
	NCT6694_TEMPERATURE_CHANNEL(23, 0x4A),	/* DTIN13 */
	NCT6694_TEMPERATURE_CHANNEL(24, 0x4C),	/* DTIN14 */
	NCT6694_TEMPERATURE_CHANNEL(25, 0x4E),	/* DTIN15 */
};

static int nct6694_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct nct6694_iio_data *data = iio_priv(indio_dev);
	unsigned char buf[2], tmp_hyst, enable_idx;
	int ret;

	if (chan->type != IIO_VOLTAGE && chan->type != IIO_TEMP)
		return -EOPNOTSUPP;

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			enable_idx = IIO_VIN_EN(chan->channel / 8);
			break;

		case IIO_TEMP:
			enable_idx = IIO_TMP_EN(chan->channel / 8);
			break;

		default:
			return -EINVAL;
		}
		ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
				       REQUEST_IIO_CMD0_OFFSET,
				       REQUEST_IIO_CMD0_LEN, enable_idx,
				       1, buf);
		if (ret)
			return -EINVAL;

		*val = buf[0] & BIT(chan->channel % 8) ? 1 : 0;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
				       chan->address, 2, 0, 2, buf);
		if (ret)
			return -EINVAL;

		switch (chan->type) {
		case IIO_VOLTAGE:	/* in micro Voltage */
			*val = buf[0] * 16;

			return IIO_VAL_INT;

		case IIO_TEMP:	/* in milli degrees Celsius */
			*val = (signed char)buf[0] * 1000;
			*val += buf[1] & 0x80 ? 500 : 0;
			*val += buf[1] & 0x40 ? 250 : 0;
			*val += buf[1] & 0x20 ? 125 : 0;

			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_HYSTERESIS:
		ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
				       REQUEST_IIO_CMD2_OFFSET,
				       REQUEST_IIO_CMD2_LEN,
				       IIO_TMP_LIMIT_IDX(chan->channel),
				       2, buf);
		if (ret)
			return -EINVAL;

		switch (chan->type) {
		case IIO_TEMP:	/* in milli degrees Celsius */
			tmp_hyst = buf[0] >> 5;
			*val = (buf[1] - tmp_hyst) * 1000;

			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int nct6694_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct nct6694_iio_data *data = iio_priv(indio_dev);
	unsigned char enable_buf[REQUEST_IIO_CMD0_LEN] = {0};
	unsigned char buf[REQUEST_IIO_CMD2_LEN] = {0};
	unsigned char delta_hyst;
	int ret;

	if (chan->type != IIO_VOLTAGE && chan->type != IIO_TEMP)
		return -EOPNOTSUPP;

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		mutex_lock(&data->iio_lock);
		ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
				       REQUEST_IIO_CMD0_OFFSET,
				       REQUEST_IIO_CMD0_LEN, 0,
				       REQUEST_IIO_CMD0_LEN,
				       enable_buf);
		if (ret)
			goto err;

		switch (chan->type) {
		case IIO_VOLTAGE:
			if (val) {
				enable_buf[IIO_VIN_EN(chan->channel / 8)] |=
				BIT(chan->channel % 8);
			} else {
				enable_buf[IIO_VIN_EN(chan->channel / 8)] &=
				~BIT(chan->channel % 8);
			}

			break;

		case IIO_TEMP:
			if (val) {
				enable_buf[IIO_TMP_EN(chan->channel / 8)] |=
				BIT(chan->channel % 8);
			} else {
				enable_buf[IIO_TMP_EN(chan->channel / 8)] &=
				~BIT(chan->channel % 8);
			}

			break;

		default:
			ret = -EINVAL;
			goto err;
		}

		ret = nct6694_write_msg(data->nct6694, REQUEST_IIO_MOD,
					REQUEST_IIO_CMD0_OFFSET,
					REQUEST_IIO_CMD0_LEN, enable_buf);
		if (ret)
			goto err;

		break;

	case IIO_CHAN_INFO_HYSTERESIS:
		switch (chan->type) {
		case IIO_TEMP:
			mutex_lock(&data->iio_lock);
			ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
					       REQUEST_IIO_CMD2_OFFSET,
					       REQUEST_IIO_CMD2_LEN, 0,
					       REQUEST_IIO_CMD2_LEN, buf);
			if (ret)
				goto err;

			delta_hyst = buf[IIO_TMP_LIMIT_IDX(chan->channel) + 1] - (u8)val;
			if (delta_hyst > 7) {
				pr_err("%s: The Hysteresis value must be less than 7!\n",
				       __func__);
				ret = -EINVAL;
				goto err;
			}

			buf[IIO_TMP_LIMIT_IDX(chan->channel)] &= IIO_CMD2_HYST_MASK;
			buf[IIO_TMP_LIMIT_IDX(chan->channel)] |= (delta_hyst << 5);
			break;

		default:
			ret = -EINVAL;
			goto err;
		}
		ret = nct6694_write_msg(data->nct6694, REQUEST_IIO_MOD,
					REQUEST_IIO_CMD2_OFFSET,
					REQUEST_IIO_CMD2_LEN, buf);
		if (ret)
			goto err;

		break;

	default:
		ret = -EINVAL;
		goto err;
	}

err:
	mutex_unlock(&data->iio_lock);
	return ret;
}

static int nct6694_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct nct6694_iio_data *data = iio_priv(indio_dev);
	unsigned char buf, ch;
	int ret;

	if (chan->type != IIO_VOLTAGE && chan->type != IIO_TEMP)
		return -EOPNOTSUPP;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (chan->type) {
		case IIO_VOLTAGE:
			ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
					       IIO_VIN_STS(chan->channel / 8),
					       1, 0, 1, &buf);
			if (ret)
				return -EINVAL;

			return !!(buf & BIT(chan->channel % 8));

		case IIO_TEMP:
			ch = (u8)IIO_TMP_STS_CH(chan->channel);

			ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
					       IIO_TMP_STS(ch / 8),
					       1, 0, 1, &buf);
			if (ret)
				return -EINVAL;

			return !!(buf & BIT(ch % 8));

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}

	return 0;
}

static int nct6694_read_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int *val, int *val2)
{
	struct nct6694_iio_data *data = iio_priv(indio_dev);
	unsigned char buf[2];
	int ret;

	if (chan->type != IIO_VOLTAGE && chan->type != IIO_TEMP)
		return -EOPNOTSUPP;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (chan->type) {
		case IIO_VOLTAGE:
			ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
					       REQUEST_IIO_CMD2_OFFSET,
					       REQUEST_IIO_CMD2_LEN,
					       IIO_VIN_LIMIT_IDX(chan->channel),
					       2, buf);
			if (ret)
				return -EINVAL;

			*val = buf[0] * 16;
			return IIO_VAL_INT;

		case IIO_TEMP:
			ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
					       REQUEST_IIO_CMD2_OFFSET,
					       REQUEST_IIO_CMD2_LEN,
					       IIO_TMP_LIMIT_IDX(chan->channel),
					       2, buf);
			if (ret)
				return -EINVAL;

			*val = (signed char)buf[1] * 1000;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_EV_DIR_FALLING:
		switch (chan->type) {
		case IIO_VOLTAGE:
			ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
					       REQUEST_IIO_CMD2_OFFSET,
					       REQUEST_IIO_CMD2_LEN,
					       IIO_VIN_LIMIT_IDX(chan->channel),
					       2, buf);
			if (ret)
				return -EINVAL;

			*val = buf[1] * 16;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int nct6694_write_event_value(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info,
				     int val, int val2)
{
	struct nct6694_iio_data *data = iio_priv(indio_dev);
	unsigned char buf[REQUEST_IIO_CMD2_LEN] = {0};
	int ret;

	if (chan->type != IIO_VOLTAGE && chan->type != IIO_TEMP)
		return -EOPNOTSUPP;

	mutex_lock(&data->iio_lock);
	ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
			       REQUEST_IIO_CMD2_OFFSET,
			       REQUEST_IIO_CMD2_LEN, 0,
			       REQUEST_IIO_CMD2_LEN, buf);
	if (ret)
		goto err;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (chan->type) {
		case IIO_VOLTAGE:
			buf[IIO_VIN_LIMIT_IDX(chan->channel)] = (u8)val;
			break;

		case IIO_TEMP:
			buf[IIO_TMP_LIMIT_IDX(chan->channel) + 1] = (s8)val;
			break;

		default:
			ret = -EINVAL;
			goto err;
		}
		break;

	case IIO_EV_DIR_FALLING:
		switch (chan->type) {
		case IIO_VOLTAGE:
			buf[IIO_VIN_LIMIT_IDX(chan->channel) + 1] = (u8)val;
			break;

		default:
			ret = -EINVAL;
			goto err;
		}
		break;

	default:
		ret = -EINVAL;
		goto err;
	}

	ret = nct6694_write_msg(data->nct6694, REQUEST_IIO_MOD,
				REQUEST_IIO_CMD2_OFFSET,
				REQUEST_IIO_CMD2_LEN, buf);
	if (ret)
		goto err;

err:
	mutex_unlock(&data->iio_lock);
	return ret;
}

static const struct iio_info nct6694_iio_info = {
	.read_raw = nct6694_read_raw,
	.write_raw = nct6694_write_raw,
	.read_event_config = nct6694_read_event_config,
	.read_event_value = nct6694_read_event_value,
	.write_event_value = nct6694_write_event_value,
};

static int nct6694_iio_init(struct nct6694_iio_data *data)
{
	unsigned char buf[REQUEST_IIO_CMD2_LEN] = {0};
	int ret;

	/* Set VIN & TMP Real Time alarm mode */
	mutex_lock(&data->iio_lock);
	ret = nct6694_read_msg(data->nct6694, REQUEST_IIO_MOD,
			       REQUEST_IIO_CMD2_OFFSET,
			       REQUEST_IIO_CMD2_LEN, 0,
			       REQUEST_IIO_CMD2_LEN, buf);
	if (ret)
		goto err;

	buf[IIO_SMI_CTRL_IDX] = 0x02;
	ret = nct6694_write_msg(data->nct6694, REQUEST_IIO_MOD,
				REQUEST_IIO_CMD2_OFFSET,
				REQUEST_IIO_CMD2_LEN, buf);
	if (ret)
		goto err;

err:
	mutex_unlock(&data->iio_lock);
	return ret;
}

static int nct6694_iio_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct nct6694_iio_data *data;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->nct6694 = nct6694;
	mutex_init(&data->iio_lock);
	platform_set_drvdata(pdev, data);

	ret = nct6694_iio_init(data);
	if (ret)
		return -EIO;

	indio_dev->name = pdev->name;
	indio_dev->channels = nct6694_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(nct6694_iio_channels);
	indio_dev->info = &nct6694_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	/* Register iio device to IIO framework */
	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register iio device!\n");
		return ret;
	}

	return 0;
}

static struct platform_driver nct6694_iio_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_iio_probe,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_iio_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_iio_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_iio_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-IIO driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
