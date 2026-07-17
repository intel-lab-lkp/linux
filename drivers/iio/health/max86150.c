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

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

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
#define MAX86150_PPG_ADC_RGE_4096	0	/* 4096 nA */
#define MAX86150_PPG_ADC_RGE_8192	1	/* 8192 nA */
#define MAX86150_PPG_ADC_RGE_16384	2	/* 16384 nA */
#define MAX86150_PPG_ADC_RGE_32768	3	/* 32768 nA */

/* PPG sample rate (SR field of PPG_CONFIG1) - single-pulse variants */
#define MAX86150_PPG_SR_SP_10HZ		0
#define MAX86150_PPG_SR_SP_20HZ		1
#define MAX86150_PPG_SR_SP_50HZ		2
#define MAX86150_PPG_SR_SP_84HZ		3
#define MAX86150_PPG_SR_SP_100HZ	4
#define MAX86150_PPG_SR_SP_200HZ	5
#define MAX86150_PPG_SR_SP_400HZ	6
#define MAX86150_PPG_SR_SP_800HZ	7
#define MAX86150_PPG_SR_SP_1000HZ	8
#define MAX86150_PPG_SR_SP_1600HZ	9
#define MAX86150_PPG_SR_SP_3200HZ	10
/* Double-pulse variants (two LED pulses averaged per sample) */
#define MAX86150_PPG_SR_DP_10HZ		11
#define MAX86150_PPG_SR_DP_20HZ		12
#define MAX86150_PPG_SR_DP_50HZ		13
#define MAX86150_PPG_SR_DP_84HZ		14
#define MAX86150_PPG_SR_DP_100HZ	15
#define MAX86150_PPG_SR_DP_200HZ	16
#define MAX86150_PPG_SR_DP_400HZ	17
#define MAX86150_PPG_SR_DP_800HZ	18
#define MAX86150_PPG_SR_DP_1000HZ	19
#define MAX86150_PPG_SR_DP_1600HZ	20
#define MAX86150_PPG_SR_DP_3200HZ	21

/* LED pulse amplitude: 0x00 = 0 mA, step ~0.8 mA, 0x3F ~= 50 mA, 0xFF ~= 200 mA */

#define MAX86150_FIFO_DEPTH		32
#define MAX86150_BYTES_PER_SLOT		3
#define MAX86150_NUM_SLOTS		3
#define MAX86150_SAMPLE_BYTES		(MAX86150_NUM_SLOTS * MAX86150_BYTES_PER_SLOT)

/* Samples available in the FIFO when the A_FULL interrupt fires */
#define MAX86150_FIFO_A_FULL_SAMPLES	17

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
 * @trig:             IIO hardware trigger backed by the device interrupt line
 * @sample_period_ns: sample period in nanoseconds (set from configured rate)
 * @fifo_raw:         scratch buffer for regmap_noinc_read() FIFO bursts; kept
 *                    in struct (heap) rather than on the stack, since stack
 *                    memory isn't guaranteed DMA-safe (e.g. CONFIG_VMAP_STACK)
 *                    and some I2C host controllers DMA the read buffer
 * @scan:             IIO push buffer; channels[] packed per active_scan_mask,
 *                    with a trailing aligned_s64 slot for the timestamp
 */
struct max86150_data {
	struct regmap		*regmap;
	struct iio_trigger	*trig;
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

	*ppg_red = (data->fifo_raw[0] & 0x07) << 16 |
		    data->fifo_raw[1] << 8 | data->fifo_raw[2];
	*ppg_ir  = (data->fifo_raw[3] & 0x07) << 16 |
		    data->fifo_raw[4] << 8 | data->fifo_raw[5];
	*ecg = sign_extend32((data->fifo_raw[6] & 0x03) << 16 |
			      data->fifo_raw[7] << 8 | data->fifo_raw[8], 17);
	return 0;
}

/*
 * Take the device out of shutdown, reset the FIFO pointers, wait for the
 * first PPG sample, and read it back.  Always returns the device to
 * shutdown before returning, whether or not the read succeeded.
 */
static int max86150_do_read_raw(struct max86150_data *data,
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
		goto out_shdn;

	ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
	if (ret)
		goto out_shdn;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
	if (ret)
		goto out_shdn;

	/*
	 * Clear stale PPG_RDY from a previous session; reading
	 * INT_STATUS1 de-asserts any pending flags so the poll
	 * below waits for a genuinely new sample.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1,
			  &ppg_rdy_status);
	if (ret)
		goto out_shdn;

	/*
	 * Poll PPG_RDY rather than sleeping a fixed interval — the
	 * internal oscillator may start slower than nominal.  25 ms
	 * covers more than two 100 Hz sample periods.
	 */
	ret = regmap_read_poll_timeout(data->regmap,
				       MAX86150_REG_INT_STATUS1,
				       ppg_rdy_status,
				       ppg_rdy_status & MAX86150_INT_PPG_RDY,
				       1000, 25000);
	if (ret)
		goto out_shdn;

	ret = max86150_read_one_sample(data, ppg_red, ppg_ir, ecg);

out_shdn:
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
	}
	default:
		return -EINVAL;
	}
}

static const struct iio_info max86150_iio_info = {
	.read_raw         = max86150_read_raw,
	.validate_trigger = iio_validate_own_trigger,
};

static int max86150_trigger_disable(struct max86150_data *data)
{
	int ret;

	ret = regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	if (ret)
		return ret;
	return regmap_set_bits(data->regmap, MAX86150_REG_SYS_CTRL,
			       MAX86150_SYS_SHDN);
}

static int max86150_trigger_enable(struct max86150_data *data)
{
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
	 * Clear a stale A_FULL latched from before this trigger was enabled;
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

static int max86150_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct max86150_data *data = iio_priv(indio_dev);

	if (state)
		return max86150_trigger_enable(data);
	return max86150_trigger_disable(data);
}

static const struct iio_trigger_ops max86150_trigger_ops = {
	.set_trigger_state = max86150_set_trigger_state,
	.validate_device   = iio_trigger_validate_own_device,
};

/*
 * Threaded IRQ handler: reads and clears INT_STATUS1 to de-assert the
 * hardware interrupt line BEFORE iio_trigger_poll() re-enables it.  This
 * prevents an IRQ storm on level-triggered lines where the line would
 * remain asserted until max86150_trigger_handler() ran later in its own
 * kthread — after IRQF_ONESHOT had already unmasked the IRQ.
 */
static irqreturn_t max86150_irq_handler(int irq, void *private)
{
	struct iio_trigger *trig = private;
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct max86150_data *data = iio_priv(indio_dev);
	unsigned int status;
	int ret;

	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1, &status);
	if (ret || !(status & MAX86150_INT_A_FULL))
		return IRQ_NONE;

	iio_trigger_poll(trig);
	return IRQ_HANDLED;
}

static irqreturn_t max86150_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *idev = pf->indio_dev;
	struct max86150_data *data = iio_priv(idev);
	unsigned int wr_ptr, rd_ptr, ovf, n_avail;
	u32 ppg_red, ppg_ir;
	s32 ecg;
	s64 t_drain = 0;
	int ret;

	/*
	 * INT_STATUS1 was already read (and the interrupt de-asserted) by
	 * max86150_irq_handler().  Read only the FIFO pointers here.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_WR_PTR, &wr_ptr);
	if (ret)
		goto done;
	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_RD_PTR, &rd_ptr);
	if (ret)
		goto done;
	ret = regmap_read(data->regmap, MAX86150_REG_OVF_COUNTER, &ovf);
	if (ret)
		goto done;

	if (ovf > 0) {
		n_avail = MAX86150_FIFO_DEPTH;
		t_drain = iio_get_time_ns(idev);
	} else {
		n_avail = (wr_ptr - rd_ptr) & (MAX86150_FIFO_DEPTH - 1);
		/*
		 * wr_ptr == rd_ptr with no overflow means either empty or
		 * exactly 32 slots filled (pointer wrapped).  Since this
		 * handler is only called when A_FULL fired, the FIFO must
		 * be full — treat as 32 available.
		 */
		if (n_avail == 0)
			n_avail = MAX86150_FIFO_DEPTH;
	}

	for (unsigned int i = 0; i < n_avail; i++) {
		unsigned int j = 0;
		s64 ts;

		if (ovf > 0)
			ts = t_drain -
			     (s64)(n_avail - 1 - i) * data->sample_period_ns;
		else
			ts = pf->timestamp +
			     ((s64)i - (MAX86150_FIFO_A_FULL_SAMPLES - 1)) *
			     data->sample_period_ns;

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);
		if (ret)
			break;

		memset(data->scan, 0, sizeof(data->scan));

		if (test_bit(MAX86150_IDX_PPG_RED, idev->active_scan_mask))
			data->scan[j++] = ppg_red;
		if (test_bit(MAX86150_IDX_PPG_IR, idev->active_scan_mask))
			data->scan[j++] = ppg_ir;
		if (test_bit(MAX86150_IDX_ECG, idev->active_scan_mask))
			data->scan[j++] = ecg;

		iio_push_to_buffers_with_timestamp(idev, data->scan, ts);
	}

done:
	iio_trigger_notify_done(idev->trig);
	return IRQ_HANDLED;
}

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
			   FIELD_PREP(MAX86150_FIFO_A_FULL,
				      MAX86150_FIFO_DEPTH -
				      MAX86150_FIFO_A_FULL_SAMPLES));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL1,
			   FIELD_PREP(MAX86150_FIFO_FD1, MAX86150_FD_LED1) |
			   FIELD_PREP(MAX86150_FIFO_FD2, MAX86150_FD_LED2));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL2,
			   FIELD_PREP(MAX86150_FIFO_FD3, MAX86150_FD_ECG) |
			   FIELD_PREP(MAX86150_FIFO_FD4, MAX86150_FD_NONE));
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_PPG_CONFIG1,
			   FIELD_PREP(MAX86150_PPG_ADC_RGE,
				      MAX86150_PPG_ADC_RGE_16384) |
			   FIELD_PREP(MAX86150_PPG_SR,
				      MAX86150_PPG_SR_SP_100HZ));
	if (ret)
		return ret;

	data->sample_period_ns = 10000000; /* matches MAX86150_PPG_SR_SP_100HZ above */

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

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable vdd supply\n");

	ret = devm_regulator_get_enable(dev, "avdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable avdd supply\n");

	ret = devm_regulator_get_enable(dev, "vref");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable vref supply\n");

	ret = devm_regulator_get_enable(dev, "leds");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable leds supply\n");

	data->regmap = devm_regmap_init_i2c(client, &max86150_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "Failed to init regmap\n");

	ret = regmap_read(data->regmap, MAX86150_REG_PART_ID, &part_id);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read part ID\n");

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

	if (client->irq > 0) {
		data->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
						    indio_dev->name,
						    iio_device_id(indio_dev));
		if (!data->trig)
			return -ENOMEM;

		data->trig->ops = &max86150_trigger_ops;
		iio_trigger_set_drvdata(data->trig, indio_dev);

		/*
		 * The device only ever drives an active-low interrupt line;
		 * there is no register to reconfigure its polarity or type,
		 * so the trigger type from firmware needs no help here.
		 */
		ret = devm_request_threaded_irq(dev, client->irq,
						NULL,
						max86150_irq_handler,
						IRQF_ONESHOT,
						"max86150", data->trig);
		if (ret)
			return ret;

		ret = devm_iio_trigger_register(dev, data->trig);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to register trigger\n");
	}

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      max86150_trigger_handler,
					      NULL);
	if (ret)
		return ret;

	/*
	 * Set the default trigger AFTER buffer setup succeeds.  Setting it
	 * before would leak the iio_trigger_get() reference if buffer setup
	 * failed: INDIO_BUFFER_TRIGGERED is not set on that path so
	 * iio_device_release() skips iio_trigger_put().
	 */
	if (data->trig)
		indio_dev->trig = iio_trigger_get(data->trig);

	return devm_iio_device_register(dev, indio_dev);
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
