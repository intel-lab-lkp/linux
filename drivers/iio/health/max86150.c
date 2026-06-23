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
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

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
#define MAX86150_REG_FIFO_DCTRL1	0x09  /* FD1[3:0]  FD2[7:4] */
#define MAX86150_REG_FIFO_DCTRL2	0x0A  /* FD3[3:0]  FD4[7:4] */
#define MAX86150_REG_SYS_CTRL		0x0D
#define MAX86150_REG_PPG_CONFIG1	0x10
#define MAX86150_REG_PPG_CONFIG2	0x11
#define MAX86150_REG_LED1_PA		0x14  /* Red LED pulse amplitude */
#define MAX86150_REG_LED2_PA		0x15  /* IR LED pulse amplitude  */
#define MAX86150_REG_ECG_CONFIG1	0x3C
#define MAX86150_REG_ECG_CONFIG3	0x3E
#define MAX86150_REG_PART_ID		0xFF

/* Field masks */
#define MAX86150_PART_ID_VAL		0x1E

/* INT_STATUS1 / INT_ENABLE1 */
#define MAX86150_INT_A_FULL		BIT(7)  /* FIFO almost full */
#define MAX86150_INT_PPG_RDY		BIT(6)  /* new PPG sample ready */

/* SYS_CTRL */
#define MAX86150_SYS_SHDN		BIT(1)
#define MAX86150_SYS_RESET		BIT(0)

/* FIFO_CONFIG */
#define MAX86150_FIFO_SMP_AVE		GENMASK(7, 5)
#define MAX86150_FIFO_ROLLOVER_EN	BIT(4)
#define MAX86150_FIFO_A_FULL		GENMASK(3, 0)

/* FIFO slot data-type codes */
#define MAX86150_FD_NONE		0x0
#define MAX86150_FD_LED1		0x1   /* Red PPG */
#define MAX86150_FD_LED2		0x2   /* IR PPG  */
#define MAX86150_FD_ECG			0x9
#define MAX86150_FIFO_FD1		GENMASK(3, 0)
#define MAX86150_FIFO_FD2		GENMASK(7, 4)
#define MAX86150_FIFO_FD3		GENMASK(3, 0)
#define MAX86150_FIFO_FD4		GENMASK(7, 4)

/* PPG_CONFIG1 */
#define MAX86150_PPG_ADC_RGE		GENMASK(7, 6)
#define MAX86150_PPG_SR			GENMASK(5, 1)

/* Geometry */
#define MAX86150_FIFO_DEPTH		32
#define MAX86150_BYTES_PER_SLOT		3    /* 24-bit word per slot */
#define MAX86150_NUM_SLOTS		3    /* Red, IR, ECG */
#define MAX86150_SAMPLE_BYTES		(MAX86150_NUM_SLOTS * MAX86150_BYTES_PER_SLOT)

/* Default hardware configuration */
#define MAX86150_LED_PA_DEFAULT		0x3F  /* ~50 mA */
#define MAX86150_PPG_SR_100HZ		4     /* PPG_SR field value for 100 Hz */
#define MAX86150_PPG_ADC_RGE_16384	2     /* 16384 nA full scale */
/* Fire A_FULL when 17 slots remain (32 - 15 = 17 samples in FIFO) */
#define MAX86150_FIFO_A_FULL_VAL	15

/* Scan element indices */
enum max86150_scan_idx {
	MAX86150_IDX_PPG_RED = 0,
	MAX86150_IDX_PPG_IR  = 1,
	MAX86150_IDX_ECG     = 2,
	MAX86150_IDX_TS,
};

/**
 * struct max86150_data - driver private state
 * @regmap:           register map for this device
 * @dev:              parent device (for dev_err logging)
 * @trig:             IIO hardware trigger backed by the device interrupt line
 * @sample_period_ns: sample period in nanoseconds (set from configured rate)
 * @fifo_raw:         DMA-safe buffer for regmap_noinc_read() FIFO bursts;
 *                    must be in struct (heap) not on the stack to satisfy
 *                    DMA mapping requirements of some I2C host controllers
 * @buf:              IIO push buffer sized for worst-case (all 3 channels
 *                    active): 3 x s32 (12 bytes) + 4-byte pad + s64
 *                    timestamp = 24 bytes.  __aligned(8) satisfies
 *                    iio_push_to_buffers_with_timestamp().
 */
struct max86150_data {
	struct regmap		*regmap;
	struct device		*dev;
	struct iio_trigger	*trig;
	u32			 sample_period_ns;
	u8			 fifo_raw[MAX86150_SAMPLE_BYTES];
	s32 buf[6] __aligned(8);
};

/* IIO channel specification */

static const struct iio_chan_spec max86150_channels[] = {
	{
		/* PPG Red LED - optical intensity, 19-bit unsigned */
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
		/* PPG IR LED - optical intensity, 19-bit unsigned */
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
		/* ECG biopotential - voltage, 18-bit signed two's complement */
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

/* Regmap configuration */

static const struct regmap_config max86150_regmap_config = {
	.reg_bits     = 8,
	.val_bits     = 8,
	.max_register = MAX86150_REG_PART_ID,
};

/* FIFO helper */

/**
 * max86150_read_one_sample - burst-read one complete 3-slot FIFO entry
 * @data:    driver state
 * @ppg_red: out - 19-bit PPG Red ADC value (unsigned)
 * @ppg_ir:  out - 19-bit PPG IR ADC value (unsigned)
 * @ecg:     out - 18-bit ECG ADC value (sign-extended to s32)
 *
 * Each FIFO entry is 9 bytes (3 slots x 3 bytes).  FIFO_DATA is a
 * streaming register - the address does not auto-increment on each
 * byte, so regmap_noinc_read() is used instead of regmap_bulk_read().
 *
 * Byte layout in the 24-bit FIFO word (MSB first):
 *   PPG 19-bit unsigned: bits [18:0], top 5 bits are always zero
 *   ECG 18-bit signed:   bits [17:0], top 6 bits are sign extension
 */
static int max86150_read_one_sample(struct max86150_data *data,
				    u32 *ppg_red, u32 *ppg_ir, s32 *ecg)
{
	int ret;

	/*
	 * Use data->fifo_raw (heap memory) not a local array so the buffer is
	 * DMA-mappable for I2C host controllers that use DMA for burst reads.
	 */
	ret = regmap_noinc_read(data->regmap, MAX86150_REG_FIFO_DATA,
				data->fifo_raw, sizeof(data->fifo_raw));
	if (ret)
		return ret;

	/* Bytes [0..2]: PPG Red - 19-bit value in bits [18:0] */
	*ppg_red = (u32)(data->fifo_raw[0] & 0x07) << 16 |
		   (u32)data->fifo_raw[1] << 8 | data->fifo_raw[2];

	/* Bytes [3..5]: PPG IR - same format */
	*ppg_ir  = (u32)(data->fifo_raw[3] & 0x07) << 16 |
		   (u32)data->fifo_raw[4] << 8 | data->fifo_raw[5];

	/* Bytes [6..8]: ECG - 18-bit signed, sign-extend to s32 */
	*ecg = sign_extend32((u32)(data->fifo_raw[6] & 0x03) << 16 |
			     (u32)data->fifo_raw[7] << 8 | data->fifo_raw[8], 17);

	return 0;
}

/* IIO read_raw */

static int max86150_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct max86150_data *data = iio_priv(indio_dev);
	u32 ppg_red, ppg_ir;
	s32 ecg;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * Claim direct mode to prevent concurrent sysfs reads from
		 * corrupting the FIFO pointers while a triggered buffer
		 * capture is active.
		 */
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		/*
		 * Single-shot path: reset FIFO pointers so the next sample
		 * read is one that arrived after this call.
		 */
		ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
		if (!ret)
			ret = regmap_write(data->regmap,
					   MAX86150_REG_OVF_COUNTER, 0);
		if (!ret)
			ret = regmap_write(data->regmap,
					   MAX86150_REG_FIFO_RD_PTR, 0);
		if (ret) {
			iio_device_release_direct(indio_dev);
			return ret;
		}

		/* Wait for one complete sample period at 100 Hz (<= 10 ms) */
		usleep_range(11000, 13000);

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);
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

/* Trigger */

/*
 * Enable or disable the FIFO almost-full interrupt when the IIO triggered
 * buffer is started or stopped.  Keeping the interrupt masked until capture
 * is active prevents a pre-existing FIFO fill from asserting the line before
 * the handler is ready to clear it.
 */
static int max86150_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct max86150_data *data = iio_priv(indio_dev);
	unsigned int mask = state ? MAX86150_INT_A_FULL : 0;
	int ret;

	if (state) {
		/*
		 * Flush stale FIFO data before enabling capture so the first
		 * samples pushed to userspace are not from before buffer enable.
		 */
		ret = regmap_write(data->regmap, MAX86150_REG_FIFO_WR_PTR, 0);
		if (ret)
			return ret;
		ret = regmap_write(data->regmap, MAX86150_REG_OVF_COUNTER, 0);
		if (ret)
			return ret;
		ret = regmap_write(data->regmap, MAX86150_REG_FIFO_RD_PTR, 0);
		if (ret)
			return ret;
	}

	return regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, mask);
}

static const struct iio_trigger_ops max86150_trigger_ops = {
	.set_trigger_state = max86150_set_trigger_state,
};

/* Triggered buffer */

/**
 * max86150_trigger_handler - threaded IRQ handler for FIFO almost-full
 *
 * Called by the IIO buffer infrastructure when the hardware trigger fires.
 * Reads INT_STATUS1 to de-assert the interrupt, then drains all available
 * FIFO samples into the IIO push buffer, packing only the channels that
 * are currently enabled in active_scan_mask.
 */
static irqreturn_t max86150_trigger_handler(int irq, void *p)
{
	struct iio_poll_func	*pf   = p;
	struct iio_dev		*idev = pf->indio_dev;
	struct max86150_data	*data = iio_priv(idev);
	unsigned int status, wr_ptr, rd_ptr, ovf;
	u32 ppg_red, ppg_ir;
	s32 ecg;
	int ret, n_avail, i, j;

	/*
	 * Reading INT_STATUS1 clears the interrupt.  Do this before touching
	 * the FIFO so the pin is de-asserted while we drain samples.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_INT_STATUS1, &status);
	if (ret)
		goto done;

	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_WR_PTR, &wr_ptr);
	if (ret)
		goto done;
	ret = regmap_read(data->regmap, MAX86150_REG_FIFO_RD_PTR, &rd_ptr);
	if (ret)
		goto done;

	/*
	 * Read OVF_COUNTER to distinguish a full FIFO from an empty one:
	 * both produce wr_ptr == rd_ptr after the pointers wrap around.
	 * If OVF_COUNTER is non-zero the FIFO has saturated; treat all 32
	 * slots as available so we drain the entire FIFO and clear the
	 * interrupt condition.
	 */
	ret = regmap_read(data->regmap, MAX86150_REG_OVF_COUNTER, &ovf);
	if (ret)
		goto done;

	if (ovf > 0)
		n_avail = MAX86150_FIFO_DEPTH;
	else
		n_avail = (wr_ptr - rd_ptr) & (MAX86150_FIFO_DEPTH - 1);

	/*
	 * Reconstruct per-sample timestamps from the IRQ capture time.
	 * iio_pollfunc_store_time() stored the IRQ time in pf->timestamp;
	 * that corresponds to the newest sample in the FIFO.  Older samples
	 * are spaced by sample_period_ns going backward in time.
	 */
	for (i = 0; i < n_avail; i++) {
		s64 ts = pf->timestamp -
			 (s64)(n_avail - 1 - i) * data->sample_period_ns;

		ret = max86150_read_one_sample(data, &ppg_red, &ppg_ir, &ecg);
		if (ret)
			break;

		/*
		 * Zero the entire buffer before packing so padding bytes
		 * between enabled channels do not leak previous sample data
		 * to userspace when fewer than 3 channels are active.
		 */
		memset(data->buf, 0, sizeof(data->buf));

		/*
		 * Pack only active channels at consecutive positions [0..j-1].
		 * iio_push_to_buffers_with_timestamp() uses scan_bytes (which
		 * accounts for the active channel count) to place the timestamp,
		 * so static indexing would misplace it when fewer than 3
		 * channels are enabled.
		 */
		j = 0;
		if (test_bit(MAX86150_IDX_PPG_RED, idev->active_scan_mask))
			data->buf[j++] = ppg_red;
		if (test_bit(MAX86150_IDX_PPG_IR, idev->active_scan_mask))
			data->buf[j++] = ppg_ir;
		if (test_bit(MAX86150_IDX_ECG, idev->active_scan_mask))
			data->buf[j++] = ecg;

		iio_push_to_buffers_with_timestamp(idev, data->buf, ts);
	}

done:
	iio_trigger_notify_done(idev->trig);
	return IRQ_HANDLED;
}

/* Chip initialisation / teardown */

static void max86150_powerdown(void *arg)
{
	struct max86150_data *data = arg;

	regmap_write(data->regmap, MAX86150_REG_INT_ENABLE1, 0);
	regmap_write(data->regmap, MAX86150_REG_SYS_CTRL, MAX86150_SYS_SHDN);
}

static int max86150_chip_init(struct max86150_data *data)
{
	int ret;

	/* Software reset; the bit self-clears within 1 ms */
	ret = regmap_write(data->regmap, MAX86150_REG_SYS_CTRL,
			   MAX86150_SYS_RESET);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	/*
	 * FIFO: no sample averaging, rollover enabled, assert A_FULL when
	 * 17 samples are in the FIFO (32 - 15 = 17 available to read).
	 */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_CONFIG,
			   MAX86150_FIFO_ROLLOVER_EN |
			   FIELD_PREP(MAX86150_FIFO_A_FULL,
				      MAX86150_FIFO_A_FULL_VAL));
	if (ret)
		return ret;

	/* Slot 1 = PPG Red (LED1), Slot 2 = PPG IR (LED2) */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL1,
			   FIELD_PREP(MAX86150_FIFO_FD1, MAX86150_FD_LED1) |
			   FIELD_PREP(MAX86150_FIFO_FD2, MAX86150_FD_LED2));
	if (ret)
		return ret;

	/* Slot 3 = ECG, Slot 4 = disabled */
	ret = regmap_write(data->regmap, MAX86150_REG_FIFO_DCTRL2,
			   FIELD_PREP(MAX86150_FIFO_FD3, MAX86150_FD_ECG) |
			   FIELD_PREP(MAX86150_FIFO_FD4, MAX86150_FD_NONE));
	if (ret)
		return ret;

	/* PPG: 100 Hz sample rate, 16384 nA ADC full-scale range */
	ret = regmap_write(data->regmap, MAX86150_REG_PPG_CONFIG1,
			   FIELD_PREP(MAX86150_PPG_ADC_RGE,
				      MAX86150_PPG_ADC_RGE_16384) |
			   FIELD_PREP(MAX86150_PPG_SR,
				      MAX86150_PPG_SR_100HZ));
	if (ret)
		return ret;

	/* LED pulse amplitudes (~50 mA) */
	ret = regmap_write(data->regmap, MAX86150_REG_LED1_PA,
			   MAX86150_LED_PA_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, MAX86150_REG_LED2_PA,
			   MAX86150_LED_PA_DEFAULT);
	if (ret)
		return ret;

	/*
	 * Record sample period for timestamp reconstruction in the trigger
	 * handler.  The PPG_SR field is fixed to 100 Hz in this driver.
	 */
	data->sample_period_ns = 10000000; /* 100 Hz = 10 ms */

	/*
	 * Do NOT enable the A_FULL interrupt here.  It is enabled and
	 * disabled via max86150_set_trigger_state() when the IIO triggered
	 * buffer is started and stopped, which prevents the edge-triggered
	 * interrupt line from asserting before the handler is registered.
	 */
	return 0;
}

/* Probe */

static int max86150_probe(struct i2c_client *client)
{
	struct iio_dev		*indio_dev;
	struct max86150_data	*data;
	unsigned int		 part_id;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data      = iio_priv(indio_dev);
	data->dev = &client->dev;

	/*
	 * Enable power supplies before any I2C access.  Both supplies are
	 * optional in the device tree; use _optional variant so probing
	 * succeeds on boards that power the device from fixed rails with no
	 * DT regulator node.
	 */
	ret = devm_regulator_get_enable_optional(&client->dev, "vdd");
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to get/enable vdd supply\n");

	ret = devm_regulator_get_enable_optional(&client->dev, "leds");
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to get/enable leds supply\n");

	data->regmap = devm_regmap_init_i2c(client, &max86150_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "Failed to initialise regmap\n");

	ret = regmap_read(data->regmap, MAX86150_REG_PART_ID, &part_id);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Cannot read part ID\n");

	if (part_id != MAX86150_PART_ID_VAL)
		return dev_err_probe(&client->dev, -ENODEV,
				     "Unexpected part ID 0x%02x (expected 0x%02x)\n",
				     part_id, MAX86150_PART_ID_VAL);

	ret = max86150_chip_init(data);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Chip initialisation failed\n");

	ret = devm_add_action_or_reset(&client->dev, max86150_powerdown, data);
	if (ret)
		return ret;

	indio_dev->name         = "max86150";
	indio_dev->channels     = max86150_channels;
	indio_dev->num_channels = ARRAY_SIZE(max86150_channels);
	indio_dev->info         = &max86150_iio_info;
	indio_dev->modes        = INDIO_DIRECT_MODE;

	/*
	 * If the device tree provides an interrupt, set up a hardware
	 * trigger so userspace can use the FIFO almost-full signal to
	 * drive capture without polling.
	 */
	if (client->irq > 0) {
		data->trig = devm_iio_trigger_alloc(&client->dev,
						    "%s-dev%d",
						    indio_dev->name,
						    iio_device_id(indio_dev));
		if (!data->trig)
			return -ENOMEM;

		data->trig->ops = &max86150_trigger_ops;
		iio_trigger_set_drvdata(data->trig, indio_dev);

		/*
		 * iio_trigger_generic_data_rdy_poll is a hard-IRQ handler
		 * that calls iio_trigger_poll() and returns IRQ_HANDLED.
		 * No IRQF_ONESHOT is needed for this edge-triggered line.
		 */
		ret = devm_request_irq(&client->dev, client->irq,
				       iio_trigger_generic_data_rdy_poll,
				       IRQF_TRIGGER_FALLING,
				       "max86150", data->trig);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "Cannot request IRQ %d\n",
					     client->irq);

		ret = devm_iio_trigger_register(&client->dev, data->trig);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "Cannot register trigger\n");

		/*
		 * Set the device's default trigger.  iio_trigger_get()
		 * increments the trigger refcount; the IIO core balances
		 * this with iio_trigger_put() in iio_dev_release() when
		 * the iio_dev is freed.  No additional devm action needed.
		 */
		indio_dev->trig = iio_trigger_get(data->trig);
	}

	ret = devm_iio_triggered_buffer_setup(&client->dev, indio_dev,
					      iio_pollfunc_store_time,
					      max86150_trigger_handler,
					      NULL);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Cannot setup triggered buffer\n");

	return devm_iio_device_register(&client->dev, indio_dev);
}

/* I2C driver table */

static const struct i2c_device_id max86150_id[] = {
	{ "max86150" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max86150_id);

static const struct of_device_id max86150_of_match[] = {
	{ .compatible = "maxim,max86150" },
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
