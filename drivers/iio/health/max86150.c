// SPDX-License-Identifier: GPL-2.0-only
/*
 * MAX86150 combined ECG and PPG biosensor driver
 *
 * Copyright (C) 2026 Md Shofiqul Islam <shofiqtest@gmail.com>
 *
 * The MAX86150 integrates two PPG optical channels (Red/IR LED) and one
 * ECG biopotential channel in a single I2C device.  Data is captured
 * through a 32-entry hardware FIFO with a configurable almost-full
 * interrupt, making it well-suited for continuous monitoring with a
 * low-power host.
 *
 * Datasheet:
 *   https://www.analog.com/media/en/technical-documentation/data-sheets/MAX86150.pdf
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

/* Register addresses */
#define MAX86150_REG_INT_STATUS1	0x00
#define MAX86150_REG_INT_STATUS2	0x01
#define MAX86150_REG_INT_ENABLE1	0x02
#define MAX86150_REG_INT_ENABLE2	0x03
#define MAX86150_REG_FIFO_WR_PTR	0x04
#define MAX86150_REG_OVF_COUNTER	0x05
#define MAX86150_REG_FIFO_RD_PTR	0x06
#define MAX86150_REG_FIFO_DATA		0x07
#define MAX86150_REG_FIFO_CONFIG	0x08
#define MAX86150_REG_FIFO_DCTRL1	0x09
#define MAX86150_REG_FIFO_DCTRL2	0x0A
#define MAX86150_REG_SYS_CTRL		0x0D
#define MAX86150_REG_PPG_CONFIG1	0x10
#define MAX86150_REG_PPG_CONFIG2	0x11
#define MAX86150_REG_LED1_PA		0x14
#define MAX86150_REG_LED2_PA		0x15
#define MAX86150_REG_ECG_CONFIG1	0x3C
#define MAX86150_REG_ECG_CONFIG3	0x3E
#define MAX86150_REG_PART_ID		0xFF

#define MAX86150_PART_ID_VAL		0x1E

/* INT_STATUS1 / INT_ENABLE1 */
#define MAX86150_INT_A_FULL		BIT(7)
#define MAX86150_INT_PPG_RDY		BIT(6)

/* SYS_CTRL */
#define MAX86150_SYS_CTRL_SHDN		BIT(1)
#define MAX86150_SYS_CTRL_RESET		BIT(0)

/* FIFO_CONFIG */
#define MAX86150_FIFO_CONFIG_SMP_AVE_MASK	GENMASK(7, 5)
#define MAX86150_FIFO_CONFIG_ROLLOVER_EN	BIT(4)
#define MAX86150_FIFO_CONFIG_A_FULL_MASK	GENMASK(3, 0)

/* FIFO slot data-type codes */
#define MAX86150_FD_NONE		0x0
#define MAX86150_FD_LED1		0x1
#define MAX86150_FD_LED2		0x2
#define MAX86150_FD_ECG			0x9

/* FIFO_DCTRL1 / FIFO_DCTRL2 */
#define MAX86150_FIFO_DCTRL_FD_LO_MASK	GENMASK(3, 0)
#define MAX86150_FIFO_DCTRL_FD_HI_MASK	GENMASK(7, 4)

/* PPG_CONFIG1 */
#define MAX86150_PPG_CONFIG1_ADC_RGE_MASK	GENMASK(7, 6)
#define MAX86150_PPG_CONFIG1_SR_MASK		GENMASK(5, 1)

#define MAX86150_FIFO_DEPTH		32
#define MAX86150_BYTES_PER_SLOT		3
#define MAX86150_NUM_SLOTS		3
#define MAX86150_SAMPLE_BYTES		(MAX86150_NUM_SLOTS * MAX86150_BYTES_PER_SLOT)

/* Fire A_FULL when 17 slots are available (32 - 15 = 17) */
#define MAX86150_FIFO_A_FULL_VAL	15

#define MAX86150_LED_PA_50MA		0x3F
#define MAX86150_PPG_SR_100HZ		4
#define MAX86150_PPG_ADC_RGE_16384	2

enum max86150_scan_idx {
	MAX86150_IDX_PPG_RED = 0,
	MAX86150_IDX_PPG_IR  = 1,
	MAX86150_IDX_ECG     = 2,
	MAX86150_IDX_TS,
};

struct max86150_data {
	struct regmap *regmap;
	u32 sample_period_ns;
	u8 fifo_raw[ALIGN(MAX86150_SAMPLE_BYTES, IIO_DMA_MINALIGN)]
		__aligned(IIO_DMA_MINALIGN);
	IIO_DECLARE_BUFFER_WITH_TS(s32, buf, 3);
};

static const struct iio_chan_spec max86150_channels[] = {
	{
		.type               = IIO_INTENSITY,
		.modified           = 1,
		.channel2           = IIO_MOD_LIGHT_RED,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index         = MAX86150_IDX_PPG_RED,
		.scan_type = {
			.sign        = 'u',
			.realbits    = 19,
			.storagebits = 32,
			.endianness  = IIO_CPU,
		},
	},
	{
		.type               = IIO_INTENSITY,
		.modified           = 1,
		.channel2           = IIO_MOD_LIGHT_IR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index         = MAX86150_IDX_PPG_IR,
		.scan_type = {
			.sign        = 'u',
			.realbits    = 19,
			.storagebits = 32,
			.endianness  = IIO_CPU,
		},
	},
	{
		.type               = IIO_VOLTAGE,
		.channel            = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index         = MAX86150_IDX_ECG,
		.scan_type = {
			.sign        = 's',
			.realbits    = 18,
			.storagebits = 32,
			.endianness  = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(MAX86150_IDX_TS),
};

static const struct regmap_config max86150_regmap_config = {
	.reg_bits     = 8,
	.val_bits     = 8,
	.max_register = MAX86150_REG_PART_ID,
};

static int max86150_read_one_sample(struct max86150_data *data,
				    u32 *ppg_red, u32 *ppg_ir, s32 *ecg)
{
	int ret;

	ret = regmap_noinc_read(data->regmap, MAX86150_REG_FIFO_DATA,
				data->fifo_raw, MAX86150_SAMPLE_BYTES);
	if (ret)
		return ret;

	*ppg_red = get_unaligned_be24(&data->fifo_raw[0]) & GENMASK(18, 0);
	*ppg_ir  = get_unaligned_be24(&data->fifo_raw[3]) & GENMASK(18, 0);
	*ecg = sign_extend32(get_unaligned_be24(&data->fifo_raw[6]) &
			     GENMASK(17, 0), 17);

	return 0;
}

static int max86150_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct max86150_data *data = iio_priv(indio_dev);
	unsigned int ppg_rdy_status;
	u32 ppg_red, ppg_ir;
	s32 ecg;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		ret = regmap_clear_bits(data->regmap, MAX86150_REG_SYS_CTRL,
					MAX86150_SYS_CTRL_SHDN);
		if (ret)
			goto out_shutdown;

		ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
		if (ret)
			goto out_shutdown;
		ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
		if (ret)
			goto out_shutdown;
		ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
		if (ret)
			goto out_shutdown;

		/*
		 * Poll PPG_RDY rather than sleeping a fixed interval -- the
		 * internal oscillator may start slower than nominal, leaving
		 * the FIFO empty if we read too early.
		 */
		ret = regmap_read_poll_timeout(data->regmap,
					       MAX86150_REG_INT_STATUS1,
					       ppg_rdy_status,
					       ppg_rdy_status & MAX86150_INT_PPG_RDY,
					       1000, 25000);
		if (ret)
			goto out_shutdown;

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);

out_shutdown:
		regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
				MAX86150_SYS_CTRL_SHDN);
		iio_device_release_direct(indio_dev);

		if (ret)
			return ret;

		switch (chan->scan_index) {
		case MAX86150_IDX_PPG_RED:
			*val = ppg_red;
			break;
		case MAX86150_IDX_PPG_IR:
			*val = ppg_ir;
			break;
		case MAX86150_IDX_ECG:
			*val = ecg;
			break;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info max86150_iio_info = {
	.read_raw = max86150_read_raw,
};

static int max86150_buffer_postenable(struct iio_dev *indio_dev)
{
	struct max86150_data *data = iio_priv(indio_dev);
	int ret;

	ret = regmap_clear_bits(data->regmap, MAX86150_REG_SYS_CTRL,
				MAX86150_SYS_CTRL_SHDN);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
	if (ret)
		goto err_shutdown;
	ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
	if (ret)
		goto err_shutdown;
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
	if (ret)
		goto err_shutdown;
	ret = regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1,
			   MAX86150_INT_A_FULL);
	if (ret)
		goto err_shutdown;
	return 0;

err_shutdown:
	regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			MAX86150_SYS_CTRL_SHDN);
	return ret;
}

static int max86150_buffer_predisable(struct iio_dev *indio_dev)
{
	struct max86150_data *data = iio_priv(indio_dev);

	regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			MAX86150_SYS_CTRL_SHDN);
	return 0;
}

static const struct iio_buffer_setup_ops max86150_buffer_setup_ops = {
	.postenable = max86150_buffer_postenable,
	.predisable = max86150_buffer_predisable,
};

static irqreturn_t max86150_interrupt_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct max86150_data *data = iio_priv(indio_dev);
	unsigned int status, wr_ptr, rd_ptr, ovf;
	u32 ppg_red, ppg_ir;
	s32 ecg;
	s64 ts;
	int ret, n_avail, i, j;

	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1, &status);
	if (ret)
		return IRQ_HANDLED;

	if (!(status & MAX86150_INT_A_FULL))
		return IRQ_NONE;

	ret = regmap_read(data->regmap, MAX86150_REG_OVF_COUNTER, &ovf);
	if (ret)
		return IRQ_HANDLED;

	if (ovf > 0) {
		/* FIFO overflowed; timestamps are unreliable - flush and discard */
		regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
		regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
		regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
		return IRQ_HANDLED;
	}

	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_WR_PTR, &wr_ptr);
	if (ret)
		return IRQ_HANDLED;
	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_RD_PTR, &rd_ptr);
	if (ret)
		return IRQ_HANDLED;

	n_avail = (wr_ptr - rd_ptr) & (MAX86150_FIFO_DEPTH - 1);
	if (!n_avail)
		return IRQ_HANDLED;

	/*
	 * Anchor timestamps to the interrupt time: sample (n_avail - 1) is
	 * the newest and corresponds to ts; earlier samples are back-calculated
	 * by one sample_period_ns per step.
	 */
	ts = ktime_get_ns();

	for (i = 0; i < n_avail; i++) {
		s64 sample_ts = ts -
			(s64)(n_avail - 1 - i) * data->sample_period_ns;

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);
		if (ret)
			break;

		j = 0;
		if (test_bit(MAX86150_IDX_PPG_RED, indio_dev->active_scan_mask))
			data->buf[j++] = ppg_red;
		if (test_bit(MAX86150_IDX_PPG_IR, indio_dev->active_scan_mask))
			data->buf[j++] = ppg_ir;
		if (test_bit(MAX86150_IDX_ECG, indio_dev->active_scan_mask))
			data->buf[j++] = ecg;

		iio_push_to_buffers_with_ts(indio_dev, data->buf,
					    sizeof(data->buf), sample_ts);
	}

	return IRQ_HANDLED;
}

static void max86150_powerdown(void *arg)
{
	struct max86150_data *data = arg;

	regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			MAX86150_SYS_CTRL_SHDN);
}

static int max86150_chip_init(struct max86150_data *data)
{
	int ret;

	/* Software reset; the bit self-clears within 1 ms */
	ret = regmap_write(data->regmap, MAX86150_REG_SYS_CTRL,
			   MAX86150_SYS_CTRL_RESET);
	if (ret)
		return ret;
	fsleep(1000);

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_CONFIG,
			   MAX86150_FIFO_CONFIG_ROLLOVER_EN |
			   FIELD_PREP(MAX86150_FIFO_CONFIG_A_FULL_MASK,
				      MAX86150_FIFO_A_FULL_VAL));
	if (ret)
		return ret;

	/* Slot 1 = PPG Red (LED1), Slot 2 = PPG IR (LED2) */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL1,
			   FIELD_PREP(MAX86150_FIFO_DCTRL_FD_LO_MASK,
				      MAX86150_FD_LED1) |
			   FIELD_PREP(MAX86150_FIFO_DCTRL_FD_HI_MASK,
				      MAX86150_FD_LED2));
	if (ret)
		return ret;

	/* Slot 3 = ECG, Slot 4 = disabled */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL2,
			   FIELD_PREP(MAX86150_FIFO_DCTRL_FD_LO_MASK,
				      MAX86150_FD_ECG) |
			   FIELD_PREP(MAX86150_FIFO_DCTRL_FD_HI_MASK,
				      MAX86150_FD_NONE));
	if (ret)
		return ret;

	/* PPG: 100 Hz sample rate, 16384 nA ADC full-scale range */
	ret = regmap_write(data->regmap, MAX86150_REG_PPG_CONFIG1,
			   FIELD_PREP(MAX86150_PPG_CONFIG1_ADC_RGE_MASK,
				      MAX86150_PPG_ADC_RGE_16384) |
			   FIELD_PREP(MAX86150_PPG_CONFIG1_SR_MASK,
				      MAX86150_PPG_SR_100HZ));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_LED1_PA, MAX86150_LED_PA_50MA);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_LED2_PA, MAX86150_LED_PA_50MA);
	if (ret)
		return ret;

	data->sample_period_ns = 10 * NSEC_PER_MSEC;

	return regmap_write(data->regmap, MAX86150_REG_SYS_CTRL,
			    MAX86150_SYS_CTRL_SHDN);
}

static int max86150_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct max86150_data *data;
	unsigned int part_id;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);

	ret = devm_regulator_get_enable(&client->dev, "vdd");
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to get/enable vdd supply\n");

	ret = devm_regulator_get_enable(&client->dev, "vled");
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to get/enable vled supply\n");

	data->regmap = devm_regmap_init_i2c(client, &max86150_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "Failed to initialise regmap\n");

	ret = regmap_read(data->regmap, MAX86150_REG_PART_ID, &part_id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Cannot read part ID\n");

	if (part_id != MAX86150_PART_ID_VAL)
		dev_warn(&client->dev,
			 "Unexpected part ID 0x%02x (expected 0x%02x)\n",
			 part_id, MAX86150_PART_ID_VAL);

	ret = max86150_chip_init(data);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Chip initialisation failed\n");

	ret = devm_add_action_or_reset(&client->dev, max86150_powerdown, data);
	if (ret)
		return ret;

	indio_dev->name = "max86150";
	indio_dev->channels = max86150_channels;
	indio_dev->num_channels = ARRAY_SIZE(max86150_channels);
	indio_dev->info = &max86150_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	if (client->irq > 0) {
		unsigned long irq_trig = irq_get_trigger_type(client->irq);

		ret = devm_iio_kfifo_buffer_setup(&client->dev, indio_dev,
						  &max86150_buffer_setup_ops);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "Cannot setup kfifo buffer\n");

		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL,
						max86150_interrupt_handler,
						irq_trig | IRQF_ONESHOT,
						"max86150", indio_dev);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "Cannot request IRQ %d\n",
					     client->irq);
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id max86150_id[] = {
	{ "max86150" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max86150_id);

static const struct of_device_id max86150_of_match[] = {
	{ .compatible = "adi,max86150" },
	{ }
};
MODULE_DEVICE_TABLE(of, max86150_of_match);

static struct i2c_driver max86150_driver = {
	.driver = {
		.name = "max86150",
		.of_match_table = max86150_of_match,
	},
	.probe = max86150_probe,
	.id_table = max86150_id,
};
module_i2c_driver(max86150_driver);

MODULE_AUTHOR("Md Shofiqul Islam <shofiqtest@gmail.com>");
MODULE_DESCRIPTION("MAX86150 ECG and PPG biosensor driver");
MODULE_LICENSE("GPL");
