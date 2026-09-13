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

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>

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

#define MAX86150_INT_A_FULL		BIT(7)
#define MAX86150_INT_PPG_RDY		BIT(6)

#define MAX86150_SYS_SHDN		BIT(1)
#define MAX86150_SYS_RESET		BIT(0)

#define MAX86150_FIFO_SMP_AVE		GENMASK(7, 5)
#define MAX86150_FIFO_ROLLOVER_EN	BIT(4)
#define MAX86150_FIFO_A_FULL		GENMASK(3, 0)

#define MAX86150_FD_NONE		0x0
#define MAX86150_FD_LED1		0x1
#define MAX86150_FD_LED2		0x2
#define MAX86150_FD_ECG			0x9
#define MAX86150_FIFO_FD1		GENMASK(3, 0)
#define MAX86150_FIFO_FD2		GENMASK(7, 4)
#define MAX86150_FIFO_FD3		GENMASK(3, 0)
#define MAX86150_FIFO_FD4		GENMASK(7, 4)

#define MAX86150_PPG_ADC_RGE		GENMASK(7, 6)
#define MAX86150_PPG_SR			GENMASK(5, 1)

/* PPG ADC full-scale range (ADC_RGE field of PPG_CONFIG1) */
#define MAX86150_PPG_ADC_RGE_4096_NA	0
#define MAX86150_PPG_ADC_RGE_8192_NA	1
#define MAX86150_PPG_ADC_RGE_16384_NA	2
#define MAX86150_PPG_ADC_RGE_32768_NA	3

/* PPG sample rate (SR field of PPG_CONFIG1) - single-pulse variants */
#define MAX86150_PPG_SR_SP_10_HZ	0
#define MAX86150_PPG_SR_SP_20_HZ	1
#define MAX86150_PPG_SR_SP_50_HZ	2
#define MAX86150_PPG_SR_SP_84_HZ	3
#define MAX86150_PPG_SR_SP_100_HZ	4
#define MAX86150_PPG_SR_SP_200_HZ	5
#define MAX86150_PPG_SR_SP_400_HZ	6
#define MAX86150_PPG_SR_SP_800_HZ	7
#define MAX86150_PPG_SR_SP_1000_HZ	8
#define MAX86150_PPG_SR_SP_1600_HZ	9
#define MAX86150_PPG_SR_SP_3200_HZ	10
/* Double-pulse variants (two LED pulses averaged per sample) */
#define MAX86150_PPG_SR_DP_10_HZ	11
#define MAX86150_PPG_SR_DP_20_HZ	12
#define MAX86150_PPG_SR_DP_50_HZ	13
#define MAX86150_PPG_SR_DP_84_HZ	14
#define MAX86150_PPG_SR_DP_100_HZ	15
#define MAX86150_PPG_SR_DP_200_HZ	16
#define MAX86150_PPG_SR_DP_400_HZ	17
#define MAX86150_PPG_SR_DP_800_HZ	18
#define MAX86150_PPG_SR_DP_1000_HZ	19
#define MAX86150_PPG_SR_DP_1600_HZ	20
#define MAX86150_PPG_SR_DP_3200_HZ	21

#define MAX86150_FIFO_DEPTH		32
#define MAX86150_BYTES_PER_SLOT		3
#define MAX86150_NUM_SLOTS		3
#define MAX86150_SAMPLE_BYTES		(MAX86150_NUM_SLOTS * MAX86150_BYTES_PER_SLOT)

/* Samples available in the FIFO when the A_FULL interrupt fires */
#define MAX86150_FIFO_A_FULL_SAMPLES	17

/* LED pulse amplitude: 0x00 = 0 mA, step ~0.8 mA, 0x3F ~= 50 mA, 0xFF ~= 200 mA */
#define MAX86150_LED_PA_DEFAULT		0x3F

enum max86150_scan_idx {
	MAX86150_IDX_PPG_RED,
	MAX86150_IDX_PPG_IR,
	MAX86150_IDX_ECG,
	MAX86150_IDX_TS,
};

/**
 * struct max86150_data - driver private state
 * @regmap:           register map for this device
 * @sample_period_ns: sample period in nanoseconds (set from configured rate)
 * @fifo_raw:         scratch buffer for regmap_noinc_read() FIFO bursts; kept
 *                    in struct (heap) rather than on the stack, since stack
 *                    memory isn't guaranteed DMA-safe (e.g. CONFIG_VMAP_STACK)
 *                    and some I2C host controllers DMA the read buffer
 * @scan:             IIO push buffer; channels[] packed per active_scan_mask
 */
struct max86150_data {
	struct regmap		*regmap;
	u32			 sample_period_ns;
	u8			 fifo_raw[MAX86150_SAMPLE_BYTES];
	IIO_DECLARE_DMA_BUFFER_WITH_TS(s32, scan, MAX86150_NUM_SLOTS);
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

static bool max86150_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case MAX86150_REG_INT_STATUS1:
	case MAX86150_REG_INT_STATUS2:
	case MAX86150_REG_FIFO_WR_PTR:
	case MAX86150_REG_OVF_COUNTER:
	case MAX86150_REG_FIFO_RD_PTR:
	case MAX86150_REG_FIFO_DATA:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config max86150_regmap_config = {
	.reg_bits     = 8,
	.val_bits     = 8,
	.max_register = MAX86150_REG_PART_ID,
	.volatile_reg  = max86150_volatile_reg,
	.cache_type   = REGCACHE_RBTREE,
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
	*ecg = sign_extend32(get_unaligned_be24(&data->fifo_raw[6]) & GENMASK(17, 0), 17);
	return 0;
}

/* Does the actual work for max86150_do_read_raw(); see that function for the shutdown wrapping. */
static int max86150_read_raw_locked(struct max86150_data *data,
				    u32 *ppg_red, u32 *ppg_ir, s32 *ecg)
{
	unsigned int ppg_rdy_status;
	int ret;

	ret = regmap_clear_bits(data->regmap, MAX86150_REG_SYS_CTRL,
				MAX86150_SYS_SHDN);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
	if (ret)
		return ret;

	/*
	 * Clear stale PPG_RDY from a previous session; reading
	 * INT_STATUS1 de-asserts any pending flags so the poll
	 * below waits for a genuinely new sample.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1,
			  &ppg_rdy_status);
	if (ret)
		return ret;

	/*
	 * Poll PPG_RDY rather than sleeping a fixed interval — the
	 * internal oscillator may start slower than nominal.  25 ms
	 * covers more than two 100 Hz sample periods.
	 */
	ret = regmap_read_poll_timeout(data->regmap,
				       MAX86150_REG_INT_STATUS1,
				       ppg_rdy_status,
				       ppg_rdy_status & MAX86150_INT_PPG_RDY,
				       USEC_PER_MSEC, 25 * USEC_PER_MSEC);
	if (ret)
		return ret;

	return max86150_read_one_sample(data, ppg_red, ppg_ir, ecg);
}

/*
 * Take the device out of shutdown, reset the FIFO pointers, wait for the
 * first PPG sample, and read it back.  Always returns the device to
 * shutdown before returning, whether or not the read succeeded.
 */
static int max86150_do_read_raw(struct max86150_data *data,
				u32 *ppg_red, u32 *ppg_ir, s32 *ecg)
{
	int ret;

	ret = max86150_read_raw_locked(data, ppg_red, ppg_ir, ecg);
	regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL, MAX86150_SYS_SHDN);
	return ret;
}

static int max86150_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct max86150_data *data = iio_priv(indio_dev);
	u32 ppg_red, ppg_ir;
	s32 ecg;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);

		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		ret = max86150_do_read_raw(data, &ppg_red, &ppg_ir, &ecg);
		if (ret)
			return ret;

		switch (chan->scan_index) {
		case MAX86150_IDX_PPG_RED:
			*val = ppg_red;
			return IIO_VAL_INT;
		case MAX86150_IDX_PPG_IR:
			*val = ppg_ir;
			return IIO_VAL_INT;
		case MAX86150_IDX_ECG:
			*val = ecg;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	}
	default:
		return -EINVAL;
	}
}

static const struct iio_info max86150_iio_info = {
	.read_raw = max86150_read_raw,
};

static int max86150_buffer_predisable(struct iio_dev *indio_dev)
{
	struct max86150_data *data = iio_priv(indio_dev);
	int ret;

	ret = regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	if (ret)
		return ret;

	return regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			       MAX86150_SYS_SHDN);
}

static int max86150_buffer_postenable(struct iio_dev *indio_dev)
{
	struct max86150_data *data = iio_priv(indio_dev);
	unsigned int dummy;
	int ret;

	ret = regmap_clear_bits(data->regmap, MAX86150_REG_SYS_CTRL,
				MAX86150_SYS_SHDN);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
	if (ret)
		goto err_shdn;

	ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
	if (ret)
		goto err_shdn;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
	if (ret)
		goto err_shdn;

	/*
	 * Clear a stale A_FULL latched from before the buffer was enabled;
	 * otherwise arming INT_ENABLE1 below fires the handler immediately
	 * against a FIFO state that was never actually seen as full.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1, &dummy);
	if (ret)
		goto err_shdn;

	ret = regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1,
			   MAX86150_INT_A_FULL);
	if (ret)
		goto err_shdn;
	return 0;

err_shdn:
	regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL, MAX86150_SYS_SHDN);
	return ret;
}

static const struct iio_buffer_setup_ops max86150_buffer_setup_ops = {
	.postenable = max86150_buffer_postenable,
	.predisable = max86150_buffer_predisable,
};

/*
 * Threaded IRQ handler (primary=NULL): clears INT_STATUS1 to de-assert the
 * line, then drains every sample currently in the FIFO and pushes each one
 * straight to the buffer.  No trigger indirection -- enabling/disabling the
 * buffer is what arms/disarms the interrupt, via
 * max86150_buffer_postenable()/_predisable() above.  Matches the direct
 * kfifo pattern max30102.c uses in this same directory.
 */
static irqreturn_t max86150_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct max86150_data *data;
	s64 irq_time;
	unsigned int status, wr_ptr, rd_ptr, ovf, n_avail;
	u32 ppg_red, ppg_ir;
	s32 ecg;
	int ret;

	data = iio_priv(indio_dev);
	irq_time = iio_get_time_ns(indio_dev);

	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1, &status);
	if (ret || !(status & MAX86150_INT_A_FULL))
		return IRQ_NONE;

	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_WR_PTR, &wr_ptr);
	if (ret)
		return IRQ_HANDLED;
	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_RD_PTR, &rd_ptr);
	if (ret)
		return IRQ_HANDLED;
	ret = regmap_read(data->regmap, MAX86150_REG_OVF_COUNTER, &ovf);
	if (ret)
		return IRQ_HANDLED;

	if (ovf > 0) {
		n_avail = MAX86150_FIFO_DEPTH;
	} else {
		n_avail = (wr_ptr - rd_ptr) & (MAX86150_FIFO_DEPTH - 1);
		/*
		 * wr_ptr == rd_ptr with no overflow means either empty or
		 * exactly 32 slots filled (pointer wrapped).  Since this
		 * handler only runs when A_FULL fired, the FIFO must be
		 * full — treat as 32 available.
		 */
		if (n_avail == 0)
			n_avail = MAX86150_FIFO_DEPTH;
	}

	for (unsigned int i = 0; i < n_avail; i++) {
		unsigned int j;
		s64 ts;

		if (ovf > 0)
			ts = irq_time -
			     (s64)(n_avail - 1 - i) * data->sample_period_ns;
		else
			ts = irq_time +
			     ((s64)i - (MAX86150_FIFO_A_FULL_SAMPLES - 1)) *
			     data->sample_period_ns;

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);
		if (ret)
			break;

		memset(data->scan, 0, sizeof(data->scan));
		j = 0;

		if (test_bit(MAX86150_IDX_PPG_RED, indio_dev->active_scan_mask))
			data->scan[j++] = ppg_red;
		if (test_bit(MAX86150_IDX_PPG_IR, indio_dev->active_scan_mask))
			data->scan[j++] = ppg_ir;
		if (test_bit(MAX86150_IDX_ECG, indio_dev->active_scan_mask))
			data->scan[j++] = ecg;

		iio_push_to_buffers_with_timestamp(indio_dev, data->scan, ts);
	}

	return IRQ_HANDLED;
}

/*
 * This is a devm_add_action_or_reset() callback, so it can't return an
 * error like the rest of this driver does -- dev_warn() is the only way
 * left to surface a failed cleanup write here.
 */
static void max86150_powerdown(void *arg)
{
	struct max86150_data *data = arg;
	struct device *dev = regmap_get_device(data->regmap);
	int ret;

	ret = regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	if (ret)
		dev_warn(dev, "Failed to disable interrupts: %d\n", ret);

	ret = regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			      MAX86150_SYS_SHDN);
	if (ret)
		dev_warn(dev, "Failed to shut down device: %d\n", ret);
}

static int max86150_chip_init(struct max86150_data *data)
{
	int ret;

	ret = regmap_write(data->regmap, MAX86150_REG_SYS_CTRL,
			   MAX86150_SYS_RESET);
	if (ret)
		return ret;

	/* SYS_RESET self-clears within 1 ms (datasheet SYS_CTRL register) */
	fsleep(10 * USEC_PER_MSEC);

	/*
	 * FIFO_A_FULL holds (FIFO depth - samples available), i.e. how many
	 * free slots remain when the interrupt should fire.
	 */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_CONFIG,
			   MAX86150_FIFO_ROLLOVER_EN |
			   FIELD_PREP_CONST(MAX86150_FIFO_A_FULL,
					    MAX86150_FIFO_DEPTH -
					    MAX86150_FIFO_A_FULL_SAMPLES));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL1,
			   FIELD_PREP_CONST(MAX86150_FIFO_FD1, MAX86150_FD_LED1) |
			   FIELD_PREP_CONST(MAX86150_FIFO_FD2, MAX86150_FD_LED2));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL2,
			   FIELD_PREP_CONST(MAX86150_FIFO_FD3, MAX86150_FD_ECG) |
			   FIELD_PREP_CONST(MAX86150_FIFO_FD4, MAX86150_FD_NONE));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_PPG_CONFIG1,
			   FIELD_PREP_CONST(MAX86150_PPG_ADC_RGE,
					    MAX86150_PPG_ADC_RGE_16384_NA) |
			   FIELD_PREP_CONST(MAX86150_PPG_SR,
					    MAX86150_PPG_SR_SP_100_HZ));
	if (ret)
		return ret;

	/* matches MAX86150_PPG_SR_SP_100_HZ above */
	data->sample_period_ns = NSEC_PER_SEC / 100;

	ret = regmap_write(data->regmap, MAX86150_REG_LED1_PA,
			   MAX86150_LED_PA_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_LED2_PA,
			   MAX86150_LED_PA_DEFAULT);
	if (ret)
		return ret;

	return regmap_write(data->regmap, MAX86150_REG_SYS_CTRL,
			    MAX86150_SYS_SHDN);
}

static const char * const max86150_supply_names[] = {
	"vdd", "avdd", "vref", "leds",
};

static int max86150_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct max86150_data *data;
	unsigned int part_id;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);

	data->regmap = devm_regmap_init_i2c(client, &max86150_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "Failed to init regmap\n");

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(max86150_supply_names),
					     max86150_supply_names);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable supplies\n");

	ret = regmap_read(data->regmap, MAX86150_REG_PART_ID, &part_id);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read part ID\n");

	/*
	 * Deliberately fatal, not a dev_warn()+continue: chip_init() below
	 * writes FIFO/PPG/LED configuration blind, with no readback. A
	 * mismatched part ID means either the wrong device is on this
	 * address or the bus itself is faulty, and letting chip_init()
	 * write to that is a worse default than refusing to bind.
	 */
	if (part_id != MAX86150_PART_ID_VAL)
		return dev_err_probe(dev, -ENODEV,
				     "Unexpected part ID 0x%02x (expected 0x%02x)\n",
				     part_id, MAX86150_PART_ID_VAL);

	ret = max86150_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "Chip initialisation failed\n");

	ret = devm_add_action_or_reset(dev, max86150_powerdown, data);
	if (ret)
		return ret;

	indio_dev->name         = "max86150";
	indio_dev->channels     = max86150_channels;
	indio_dev->num_channels = ARRAY_SIZE(max86150_channels);
	indio_dev->info         = &max86150_iio_info;
	indio_dev->modes        = INDIO_DIRECT_MODE;

	ret = devm_iio_kfifo_buffer_setup(dev, indio_dev,
					  &max86150_buffer_setup_ops);
	if (ret)
		return ret;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						max86150_irq_handler,
						IRQF_ONESHOT,
						"max86150", indio_dev);
		if (ret)
			return ret;
	}

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id max86150_id[] = {
	{ .name = "max86150" },
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
		.name           = "max86150",
		.of_match_table = max86150_of_match,
	},
	.probe    = max86150_probe,
	.id_table = max86150_id,
};
module_i2c_driver(max86150_driver);

MODULE_AUTHOR("Md Shofiqul Islam <shofiqtest@gmail.com>");
MODULE_DESCRIPTION("MAX86150 ECG and PPG biosensor driver");
MODULE_LICENSE("GPL");
