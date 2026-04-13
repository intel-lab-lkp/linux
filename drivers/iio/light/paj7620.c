// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO Driver for PixArt PAJ7620 Gesture Sensor
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

/*
 * Register Bank select
 */
#define PAJ_BANK_SELECT       0xEF  /* Bank0=0x00,Bank1=0x01 */

/*
 * Register Bank 0
 */
#define PAJ_SUSPEND           0x03
#define PAJ_INT_FLAG1_MASK    0x41
#define PAJ_INT_FLAG2_MASK    0x42
#define PAJ_INT_FLAG1         0x43  /* Gesture detection interrupt flag */
#define PAJ_INT_FLAG2         0x44  /* Gesture/PS detection interrupt flag */
#define PAJ_STATE             0x45
#define PAJ_PS_HIGH_THRESHOLD 0x69
#define PAJ_PS_LOW_THRESHOLD  0x6A
#define PAJ_SLEEP_BANK0	      0x65
#define PAJ_SLEEP_BANK1	      0x05

/*
 * Gesture detection interrupt flag (Table reference)
 */
#define PAJ_UP                0x01
#define PAJ_DOWN              0x02
#define PAJ_LEFT              0x04
#define PAJ_RIGHT             0x08
#define PAJ_FORWARD           0x10
#define PAJ_BACKWARD          0x20
#define PAJ_CLOCKWISE         0x40
#define PAJ_COUNT_CLOCKWISE   0x80
#define PAJ_WAVE              0x100

/* Regmap config */
static const struct regmap_config paj_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF,
};

/*
 * The following arrays contain undocumented register sequences required to
 * initialize the sensor's internal DSP and gesture engine.
 * These were derived from vendor reference code and verified via testing.
 */
static const struct reg_sequence Init_Register[] = {
	{ 0xEF, 0x00 }, { 0x37, 0x07 }, { 0x38, 0x17 }, { 0x39, 0x06 },
	{ 0x41, 0x00 }, { 0x42, 0x00 }, { 0x46, 0x2D }, { 0x47, 0x0F },
	{ 0x48, 0x3C }, { 0x49, 0x00 }, { 0x4A, 0x1E }, { 0x4C, 0x20 },
	{ 0x51, 0x10 }, { 0x5E, 0x10 }, { 0x60, 0x27 }, { 0x80, 0x42 },
	{ 0x81, 0x44 }, { 0x82, 0x04 }, { 0x8B, 0x01 }, { 0x90, 0x06 },
	{ 0x95, 0x0A }, { 0x96, 0x0C }, { 0x97, 0x05 }, { 0x9A, 0x14 },
	{ 0x9C, 0x3F }, { 0xA5, 0x19 }, { 0xCC, 0x19 }, { 0xCD, 0x0B },
	{ 0xCE, 0x13 }, { 0xCF, 0x64 }, { 0xD0, 0x21 }, { 0xEF, 0x01 },
	{ 0x02, 0x0F }, { 0x03, 0x10 }, { 0x04, 0x02 }, { 0x25, 0x01 },
	{ 0x27, 0x39 }, { 0x28, 0x7F }, { 0x29, 0x08 }, { 0x3E, 0xFF },
	{ 0x5E, 0x3D }, { 0x65, 0x96 }, { 0x67, 0x97 }, { 0x69, 0xCD },
	{ 0x6A, 0x01 }, { 0x6D, 0x2C }, { 0x6E, 0x01 }, { 0x72, 0x01 },
	{ 0x73, 0x35 }, { 0x74, 0x00 }, { 0x77, 0x01 },
};

/*
 * Specific configuration overrides required to enable the internal
 * 9-gesture state machine.
 */
static const struct reg_sequence Init_Gesture_Array[] = {
	{ 0xEF, 0x00 }, { 0x41, 0x00 }, { 0x42, 0x00 }, { 0xEF, 0x00 },
	{ 0x48, 0x3C }, { 0x49, 0x00 }, { 0x51, 0x10 }, { 0x83, 0x20 },
	{ 0x9F, 0xF9 }, { 0xEF, 0x01 }, { 0x01, 0x1E }, { 0x02, 0x0F },
	{ 0x03, 0x10 }, { 0x04, 0x02 }, { 0x41, 0x40 }, { 0x43, 0x30 },
	{ 0x65, 0x96 }, { 0x66, 0x00 }, { 0x67, 0x97 }, { 0x68, 0x01 },
	{ 0x69, 0xCD }, { 0x6A, 0x01 }, { 0x6B, 0xB0 }, { 0x6C, 0x04 },
	{ 0x6D, 0x2C }, { 0x6E, 0x01 }, { 0x74, 0x00 }, { 0xEF, 0x00 },
	{ 0x41, 0xFF }, { 0x42, 0x01 },
};

struct paj7620_data {
	struct regmap *regmap;
	struct i2c_client *client;
	struct mutex lock;
	int last_gesture; /* cache gesture value (1-8) */
};

static const struct iio_event_spec paj7620_events[] = {
	{
		.type = IIO_EV_TYPE_GESTURE,
		.dir = IIO_EV_DIR_NONE,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

#define PAJ7620_CHAN(chan_idx)                                                 \
	{                                                                      \
		.type = IIO_ACTIVITY, .indexed = 1, .channel = chan_idx,       \
		.event_spec = paj7620_events,                                  \
		.num_event_specs = ARRAY_SIZE(paj7620_events),                 \
	}

static const struct iio_chan_spec paj7620_channels[] = {
	PAJ7620_CHAN(1), /* Right */
	PAJ7620_CHAN(2), /* Left */
	PAJ7620_CHAN(3), /* Up */
	PAJ7620_CHAN(4), /* Down */
	PAJ7620_CHAN(5), /* Forward */
	PAJ7620_CHAN(6), /* Backward */
	PAJ7620_CHAN(7), /* Clockwise */
	PAJ7620_CHAN(8), /* Anti-Clockwise */
};

/* Read function */
static int paj7620_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct paj7620_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		*val = data->last_gesture;
		mutex_unlock(&data->lock);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int paj7620_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct paj7620_data *data = iio_priv(indio_dev);
	unsigned int val1, val2;
	int ret;
	u16 mask;

	/* Map the 1-based channel index back to the 0-based bit shift */
	int bit_shift = chan->channel - 1;

	mutex_lock(&data->lock);
	/* Force Bank 0 */
	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x00);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}

	/* Read current interrupt mask from chip */
	ret = regmap_read(data->regmap, PAJ_INT_FLAG1_MASK, &val1);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}

	ret = regmap_read(data->regmap, PAJ_INT_FLAG2_MASK, &val2);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;

	/* Combine into a single 16-bit mask */
	mask = (val2 << 8) | val1;

	/* Check if the specific bit for this channel is enabled */
	return !!(mask & BIT(bit_shift));
}

static int paj7620_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir, bool state)
{
	struct paj7620_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);

	/* 1. Force Bank 0 */
	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x00);
	if (ret < 0)
		goto out;

	if (state) {
		/* 2. Enable ALL gesture interrupts on the chip */
		ret = regmap_write(data->regmap, PAJ_INT_FLAG1_MASK, 0xFF);
		if (ret < 0)
			goto out;
		ret = regmap_write(data->regmap, PAJ_INT_FLAG2_MASK, 0x01);
	} else {
		/* 3. Mute ALL gesture interrupts on the chip */
		ret = regmap_write(data->regmap, PAJ_INT_FLAG1_MASK, 0x00);
		if (ret < 0)
			goto out;
		ret = regmap_write(data->regmap, PAJ_INT_FLAG2_MASK, 0x00);
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static const struct iio_info paj7620_info = {
	.read_raw = paj7620_read_raw,
	.read_event_config = paj7620_read_event_config,
	.write_event_config = paj7620_write_event_config,
};

static int paj7620_decode_gesture(int val)
{
	int active_gestures = val & 0xFF;

	if (!active_gestures)
		return 0;

	/*
	 * __ffs returns the index of the first set bit (0 to 31).
	 * Bit 0 (Right) returns 0 -> map to channel 1
	 * Bit 1 (Left) returns 1 -> map to channel 2
	 * ... and so on.
	 */
	return __ffs(active_gestures) + 1;
}

static int paj7620_read_gesture(struct paj7620_data *data)
{
	int g1 = 0, g2 = 0, ret;

	/* 1. FORCE Bank 0 before every read */
	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x00);
	if (ret < 0)
		return ret;

	/* 2. Read BOTH registers to clear the sensor's internal interrupt state */
	ret = regmap_read(data->regmap, PAJ_INT_FLAG1, &g1);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, PAJ_INT_FLAG2, &g2);
	if (ret < 0)
		return ret;

	/* 3. The gesture is a 16-bit combined value */
	return ((g2 << 8) | g1);
}

static irqreturn_t paj7620_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct paj7620_data *data = iio_priv(indio_dev);
	int raw_val, decoded;
	irqreturn_t status = IRQ_NONE;

	mutex_lock(&data->lock);
	raw_val = paj7620_read_gesture(data);
	if (raw_val > 0) {
		dev_dbg(&data->client->dev, "gesture = %d\n", raw_val);
		decoded = paj7620_decode_gesture(raw_val);
		if (decoded >= 1 && decoded <= 8) {
			data->last_gesture = decoded;
			iio_push_event(indio_dev,
			IIO_MOD_EVENT_CODE(IIO_ACTIVITY,
						decoded,
						IIO_NO_MOD,
						IIO_EV_TYPE_GESTURE,
						IIO_EV_DIR_NONE),
				       iio_get_time_ns(indio_dev));
			status = IRQ_HANDLED;
		}
	}
	mutex_unlock(&data->lock);
	return status;
}

static int paj7620_init(struct paj7620_data *data)
{
	int state = 0, ret, i;

	/* 1. Wake-up sequence: Read register 0x00 until it returns 0x20 */
	for (i = 0; i < 10; i++) {
		ret = regmap_read(data->regmap, 0x00, &state);
		if (ret >= 0 && state == 0x20)
			break;
		usleep_range(1000, 2000);
	}

	if (state != 0x20) {
		dev_err(&data->client->dev, "Sensor wake-up failed (0x%02x)\n", state);
		return -ENODEV;
	}

	/* 2. Blast full register array into PAJ7620 instantly */
	ret = regmap_multi_reg_write(data->regmap, Init_Register,
				     ARRAY_SIZE(Init_Register));
	if (ret < 0) {
		dev_err(&data->client->dev, "Multi-reg write failed (%d)\n", ret);
		return ret;
	}

	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x00);
	if (ret < 0)
		return ret;

	ret = regmap_multi_reg_write(data->regmap, Init_Gesture_Array,
				     ARRAY_SIZE(Init_Gesture_Array));
	if (ret < 0) {
		dev_err(&data->client->dev, "Multi-reg write failed (%d)\n", ret);
		return ret;
	}

	dev_info(&data->client->dev, "Gesture Sensor Initialized OK\n");
	return 0;
}

static int paj7620_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct paj7620_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	i2c_set_clientdata(client, indio_dev);
	/* Initialize Regmap */
	data->regmap = devm_regmap_init_i2c(client, &paj_regmap_config);
	if (IS_ERR(data->regmap)) {
		dev_err(&client->dev, "Failed to initialize regmap\n");
		return PTR_ERR(data->regmap);
	}

	indio_dev->info = &paj7620_info;
	indio_dev->channels = paj7620_channels;
	indio_dev->num_channels = ARRAY_SIZE(paj7620_channels);
	indio_dev->name = "paj7620";
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = paj7620_init(data);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					paj7620_irq_handler, IRQF_ONESHOT,
					"paj7620_irq", indio_dev);
	if (ret)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int paj7620_suspend(struct device *dev)
{
	int ret;
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct paj7620_data *data = iio_priv(indio_dev);

	/* Bank 0 suspend */
	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x00);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, PAJ_SLEEP_BANK0, 0x01);
	if (ret)
		return ret;
	/* Bank 1 suspend */
	ret = regmap_write(data->regmap, PAJ_BANK_SELECT, 0x01);
	if (ret)
		return ret;
	ret = regmap_write(data->regmap, PAJ_SLEEP_BANK1, 0x01);
	if (ret)
		return ret;

	dev_info(dev, "PAJ7620 entering low power suspend mode\n");
	return 0;
}

static int paj7620_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct paj7620_data *data = iio_priv(indio_dev);

	dev_info(dev, "PAJ7620 waking up from suspend\n");

	/* Re-run full hardware initialization */
	return paj7620_init(data);
}

DEFINE_SIMPLE_DEV_PM_OPS(paj7620_pm_ops, paj7620_suspend, paj7620_resume);

static const struct of_device_id paj7620_of_match[] = {
	{ .compatible = "pixart,paj7620" },
	{ }
};
MODULE_DEVICE_TABLE(of, paj7620_of_match);

static const struct i2c_device_id paj7620_id[] = {
	{ "paj7620", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, paj7620_id);

static struct i2c_driver paj7620_driver = {
	.driver = {
		.name = "paj7620",
		.of_match_table = paj7620_of_match,
		.pm = pm_ptr(&paj7620_pm_ops),
	},
	.probe = paj7620_probe,
	.id_table = paj7620_id,
};
module_i2c_driver(paj7620_driver);

MODULE_AUTHOR("Harpreet Saini");
MODULE_DESCRIPTION("IIO Driver for PixArt PAJ7620");
MODULE_LICENSE("GPL");
