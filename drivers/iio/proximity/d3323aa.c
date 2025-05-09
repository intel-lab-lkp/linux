// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Nicera D3-323-AA PIR sensor.
 *
 * Copyright (C) 2025 Axis Communications AB
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>

#define D3323AA_DRV_NAME "d3323aa"

/*
 * Register bitmap.
 * For some reason the first bit is denoted as F37 in the datasheet, the second
 * as F38 and so on. Note the gap between F60 and F64.
 */
#define D3323AA_REG_BIT_SLAVEA1		0	/* F37. */
#define D3323AA_REG_BIT_SLAVEA2		1	/* F38. */
#define D3323AA_REG_BIT_SLAVEA3		2	/* F39. */
#define D3323AA_REG_BIT_SLAVEA4		3	/* F40. */
#define D3323AA_REG_BIT_SLAVEA5		4	/* F41. */
#define D3323AA_REG_BIT_SLAVEA6		5	/* F42. */
#define D3323AA_REG_BIT_SLAVEA7		6	/* F43. */
#define D3323AA_REG_BIT_SLAVEA8		7	/* F44. */
#define D3323AA_REG_BIT_SLAVEA9		8	/* F45. */
#define D3323AA_REG_BIT_SLAVEA10	9	/* F46. */
#define D3323AA_REG_BIT_DETLVLABS0	10	/* F47. */
#define D3323AA_REG_BIT_DETLVLABS1	11	/* F48. */
#define D3323AA_REG_BIT_DETLVLABS2	12	/* F49. */
#define D3323AA_REG_BIT_DETLVLABS3	13	/* F50. */
#define D3323AA_REG_BIT_DETLVLABS4	14	/* F51. */
#define D3323AA_REG_BIT_DETLVLABS5	15	/* F52. */
#define D3323AA_REG_BIT_DETLVLABS6	16	/* F53. */
#define D3323AA_REG_BIT_DETLVLABS7	17	/* F54. */
#define D3323AA_REG_BIT_DSLP		18	/* F55. */
#define D3323AA_REG_BIT_FSTEP0		19	/* F56. */
#define D3323AA_REG_BIT_FSTEP1		20	/* F57. */
#define D3323AA_REG_BIT_FILSEL0		21	/* F58. */
#define D3323AA_REG_BIT_FILSEL1		22	/* F59. */
#define D3323AA_REG_BIT_FILSEL2		23	/* F60. */
#define D3323AA_REG_BIT_FDSET		24	/* F64. */
#define D3323AA_REG_BIT_F65		25
#define D3323AA_REG_BIT_F87		(D3323AA_REG_BIT_F65 + (87 - 65))

#define D3323AA_REG_NR_BITS (D3323AA_REG_BIT_F87 - D3323AA_REG_BIT_SLAVEA1 + 1)
#define D3323AA_THRESH_REG_NR_BITS                                             \
	(D3323AA_REG_BIT_DETLVLABS7 - D3323AA_REG_BIT_DETLVLABS0 + 1)
#define D3323AA_FILTER_TYPE_NR_BITS                                            \
	(D3323AA_REG_BIT_FILSEL2 - D3323AA_REG_BIT_FILSEL0 + 1)
#define D3323AA_FILTER_GAIN_REG_NR_BITS                                        \
	(D3323AA_REG_BIT_FSTEP1 - D3323AA_REG_BIT_FSTEP0 + 1)

#define D3323AA_THRESH_DEFAULT_VAL 56
#define D3323AA_FILTER_GAIN_DEFAULT_VAL 2

/*
 * The pattern is 0b01101, but store it reversed (0b10110) due to writing from
 * LSB on the wire (c.f. d3323aa_write_settings()).
 */
#define D3323AA_SETTING_END_PATTERN 0x16
#define D3323AA_SETTING_END_PATTERN_NR_BITS 5

/*
 * Device should be ready for configuration after this many milliseconds.
 * Datasheet mentions "approx. 1.2 s". Measurements show around 1.23 s,
 * therefore add 100 ms of slack.
 */
#define D3323AA_RESET_TIMEOUT (1200 + 100)

/*
 * The configuration of the device (write and read) should be done within this
 * many milliseconds.
 */
#define D3323AA_CONFIG_TIMEOUT 1400

/* Number of IRQs needed for configuration stage after reset. */
#define D3323AA_IRQ_RESET_COUNT 2

/*
 * High-pass filter cutoff frequency for the band-pass filter. There is a
 * corresponding low-pass cutoff frequency for each of the filter types
 * (denoted A, B, C and D in the datasheet). The index in this array matches
 * that corresponding value in d3323aa_lp_filter_freq.
 * Note that this represents a fractional value (e.g. the first value
 * corresponds to 4 / 10 = 0.4 Hz).
 */
static const int d3323aa_hp_filter_freq[][2] = {
	{ 4, 10 },
	{ 3, 10 },
	{ 3, 10 },
	{ 1, 100 },
};

/*
 * Low-pass filter cutoff frequency for the band-pass filter. There is a
 * corresponding high-pass cutoff frequency for each of the filter types
 * (denoted A, B, C and D in the datasheet). The index in this array matches
 * that corresponding value in d3323aa_hp_filter_freq.
 * Note that this represents a fractional value (e.g. the first value
 * corresponds to 27 / 10 = 2.7 Hz).
 */
static const int d3323aa_lp_filter_freq[][2] = {
	{ 27, 10 },
	{ 15, 10 },
	{ 5, 1 },
	{ 100, 1 },
};

/*
 * Register bitmap values for filter types (denoted A, B, C and D in the
 * datasheet). The index in this array matches the corresponding value in
 * d3323aa_lp_filter_freq (which in turn matches d3323aa_hp_filter_freq). For
 * example, the first value 7 corresponds to 2.7 Hz low-pass and 0.4 Hz
 * high-pass cutoff frequency.
 */
static const int d3323aa_lp_filter_regval[] = {
	7,
	0,
	1,
	2,
};

/*
 * This is denoted as "step" in datasheet and corresponds to the gain at peak
 * for the band-pass filter. The index in this array is the corresponding index
 * in d3323aa_filter_gain_regval for the register bitmap value.
 */
static const int d3323aa_filter_gain[] = {
	1,
	2,
	3,
};

/*
 * Register bitmap values for the filter gain. The index in this array is the
 * corresponding index in d3323aa_filter_gain for the gain value.
 */
static const u8 d3323aa_filter_gain_regval[] = {
	1,
	3,
	0,
};

struct d3323aa_data {
	struct completion reset_completion;
	/*
	 *  Since the setup process always requires a complete write of the
	 *  _whole_ register bitmap, we need to synchronize it with a lock.
	 */
	struct mutex regmap_lock;
	atomic_t irq_reset_count;
	unsigned int irq;

	struct device *dev;

	/* Supply voltage. */
	struct gpio_desc *gpiod_vdd;
	/* Input clock or output detection signal (Vout). */
	struct gpio_desc *gpiod_clk_vout;
	/* Input (setting) or output data. */
	struct gpio_desc *gpiod_data;

	DECLARE_BITMAP(regmap, D3323AA_REG_NR_BITS);

	/*
	 * Indicator for operational mode (configuring or detecting), i.e.
	 * d3323aa_irq_detection() registered or not.
	 */
	bool detecting;
};

static int d3323aa_read_settings(struct iio_dev *indio_dev,
				 unsigned long *regmap)
{
	struct d3323aa_data *data = iio_priv(indio_dev);
	size_t i;
	int ret;

	/* Bit bang the clock and data pins. */
	ret = gpiod_direction_output(data->gpiod_clk_vout, 0);
	if (ret)
		return ret;

	ret = gpiod_direction_input(data->gpiod_data);
	if (ret)
		return ret;

	dev_dbg(data->dev, "Reading settings...\n");

	for (i = 0; i < D3323AA_REG_NR_BITS; ++i) {
		/* Clock frequency needs to be 1 kHz. */
		gpiod_set_value(data->gpiod_clk_vout, 1);
		udelay(500);

		/* The data seems to change when clock signal is high. */
		if (gpiod_get_value(data->gpiod_data))
			set_bit(i, regmap);

		gpiod_set_value(data->gpiod_clk_vout, 0);
		udelay(500);
	}

	/* The first bit (F37) is just dummy data. Discard it. */
	clear_bit(0, regmap);

	/* Datasheet says to wait 30 ms after reading the settings. */
	msleep(30);

	return 0;
}

static int d3323aa_write_settings(struct iio_dev *indio_dev,
				  const unsigned long *regmap)
{
	DECLARE_BITMAP(end_pattern, D3323AA_SETTING_END_PATTERN_NR_BITS);
	struct d3323aa_data *data = iio_priv(indio_dev);
	size_t i;
	int ret;

	/* Bit bang the clock and data pins. */
	ret = gpiod_direction_output(data->gpiod_clk_vout, 0);
	if (ret)
		return ret;

	ret = gpiod_direction_output(data->gpiod_data, 0);
	if (ret)
		return ret;

	dev_dbg(data->dev, "Writing settings...\n");

	/* First bit (F37) is not used when writing the register map. */
	for (i = 1; i < D3323AA_REG_NR_BITS; ++i) {
		gpiod_set_value(data->gpiod_data, test_bit(i, regmap));

		/* Clock frequency needs to be 1 kHz. */
		gpiod_set_value(data->gpiod_clk_vout, 1);
		udelay(500);
		gpiod_set_value(data->gpiod_clk_vout, 0);
		udelay(500);
	}

	/* Compulsory end pattern. */
	bitmap_write(end_pattern, D3323AA_SETTING_END_PATTERN, 0,
		     D3323AA_SETTING_END_PATTERN_NR_BITS);
	for (i = 0; i < D3323AA_SETTING_END_PATTERN_NR_BITS; ++i) {
		gpiod_set_value(data->gpiod_data, test_bit(i, end_pattern));

		gpiod_set_value(data->gpiod_clk_vout, 1);
		udelay(500);
		gpiod_set_value(data->gpiod_clk_vout, 0);
		udelay(500);
	}

	/* Datasheet says to wait 30 ms after writing the settings. */
	msleep(30);

	return 0;
}

static irqreturn_t d3323aa_irq_detection(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct d3323aa_data *data = iio_priv(indio_dev);
	enum iio_event_direction dir;
	int val;

	val = gpiod_get_value(data->gpiod_clk_vout);
	if (val < 0) {
		dev_err_ratelimited(data->dev,
				    "Could not read from GPIO clk-vout (%d)\n",
				    val);
		return IRQ_HANDLED;
	}

	dir = val ? IIO_EV_DIR_RISING : IIO_EV_DIR_FALLING;
	iio_push_event(indio_dev,
		       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
					    IIO_EV_TYPE_THRESH, dir),
		       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static irqreturn_t d3323aa_irq_reset(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct d3323aa_data *data = iio_priv(indio_dev);
	int count = atomic_inc_return(&data->irq_reset_count);

	if (count == D3323AA_IRQ_RESET_COUNT)
		complete(&data->reset_completion);

	return IRQ_HANDLED;
}

static int d3323aa_reset(struct iio_dev *indio_dev)
{
	struct d3323aa_data *data = iio_priv(indio_dev);
	long time;
	int ret;

	/*
	 * Datasheet says VDD needs to be low at least for 30 ms. Let's add a
	 * couple more to allow VDD to completely discharge as well.
	 */
	gpiod_set_value(data->gpiod_vdd, 0);
	msleep(30 + 5);

	/*
	 * After setting VDD to high, the device signals with
	 * D3323AA_IRQ_RESET_COUNT falling edges on CLK/Vout that it is now
	 * ready for configuration. Datasheet says that this should happen
	 * within D3323AA_RESET_TIMEOUT ms. Count these two edges within that
	 * timeout.
	 */
	atomic_set(&data->irq_reset_count, 0);
	reinit_completion(&data->reset_completion);

	ret = gpiod_direction_input(data->gpiod_clk_vout);
	if (ret)
		return ret;

	dev_dbg(data->dev, "Resetting...\n");

	gpiod_set_value(data->gpiod_vdd, 1);

	/*
	 * Datasheet doesn't mention this but measurements have shown that
	 * CLK/Vout signal slowly ramps up during the first 1.5 ms after reset.
	 * This means that the digital signal will have bogus values during this
	 * period. Let's wait for this ramp-up before counting the "real"
	 * falling edges.
	 */
	usleep_range(2000, 5000);

	if (data->detecting) {
		/*
		 * Device had previously been set up and was in operational
		 * mode. Thus, free that detection IRQ handler before requesting
		 * the reset IRQ handler.
		 */
		free_irq(data->irq, indio_dev);
		data->detecting = false;
	}

	ret = request_irq(data->irq, d3323aa_irq_reset, IRQF_TRIGGER_FALLING,
			  dev_name(data->dev), indio_dev);
	if (ret)
		return ret;

	time = wait_for_completion_killable_timeout(
		&data->reset_completion,
		msecs_to_jiffies(D3323AA_RESET_TIMEOUT));
	free_irq(data->irq, indio_dev);
	if (time == 0) {
		return -ETIMEDOUT;
	} else if (time < 0) {
		/* Got interrupted. */
		return time;
	}

	dev_dbg(data->dev, "Reset completed\n");

	return 0;
}

static int d3323aa_setup(struct iio_dev *indio_dev, const unsigned long *regmap)
{
	DECLARE_BITMAP(read_regmap, D3323AA_REG_NR_BITS);
	struct d3323aa_data *data = iio_priv(indio_dev);
	unsigned long start_time;
	int ret;

	ret = d3323aa_reset(indio_dev);
	if (ret) {
		if (ret != -ERESTARTSYS)
			dev_err(data->dev, "Could not reset device (%d)\n",
				ret);

		return ret;
	}

	/*
	 * Datasheet says to wait 10 us before setting the configuration.
	 * Moreover, the total configuration should be done within
	 * D3323AA_CONFIG_TIMEOUT ms. Clock it.
	 */
	usleep_range(10, 20);
	start_time = jiffies;

	ret = d3323aa_write_settings(indio_dev, regmap);
	if (ret) {
		dev_err(data->dev, "Could not write settings (%d)\n", ret);
		return ret;
	}

	ret = d3323aa_read_settings(indio_dev, read_regmap);
	if (ret) {
		dev_err(data->dev, "Could not read settings (%d)\n", ret);
		return ret;
	}

	if (time_is_before_jiffies(start_time +
				   msecs_to_jiffies(D3323AA_CONFIG_TIMEOUT))) {
		dev_err(data->dev, "Could not set up configuration in time\n");
		return -EAGAIN;
	}

	/* Check if settings were set successfully. */
	if (!bitmap_equal(regmap, read_regmap, D3323AA_REG_NR_BITS)) {
		dev_err(data->dev, "Settings data mismatch\n");
		return -EIO;
	}

	/* Now in operational mode. */
	ret = gpiod_direction_input(data->gpiod_clk_vout);
	if (ret) {
		dev_err(data->dev,
			"Could not set GPIO clk-vout as input (%d)\n", ret);
		return ret;
	}

	ret = gpiod_direction_input(data->gpiod_data);
	if (ret) {
		dev_err(data->dev, "Could not set GPIO data as input (%d)\n",
			ret);
		return ret;
	}

	ret = request_irq(data->irq, d3323aa_irq_detection,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  dev_name(data->dev), indio_dev);
	if (ret) {
		dev_err(data->dev, "Could not request IRQ for detection (%d)\n",
			ret);
		return ret;
	}

	bitmap_copy(data->regmap, regmap, D3323AA_REG_NR_BITS);
	data->detecting = true;

	dev_dbg(data->dev, "Setup done\n");

	return 0;
}

static int d3323aa_get_hp_filter_freq(unsigned long *regmap, int *val,
				      int *val2)
{
	size_t idx;
	u8 regval;

	regval = bitmap_read(regmap, D3323AA_REG_BIT_FILSEL0,
			     D3323AA_FILTER_TYPE_NR_BITS);
	for (idx = 0; idx < ARRAY_SIZE(d3323aa_lp_filter_regval); ++idx) {
		if (d3323aa_lp_filter_regval[idx] == regval)
			break;
	}

	if (idx == ARRAY_SIZE(d3323aa_lp_filter_regval))
		return -EINVAL;

	*val = d3323aa_hp_filter_freq[idx][0];
	*val2 = d3323aa_hp_filter_freq[idx][1];

	return 0;
}

static int d3323aa_get_lp_filter_freq(unsigned long *regmap, int *val,
				      int *val2)
{
	size_t idx;
	u8 regval;

	regval = bitmap_read(regmap, D3323AA_REG_BIT_FILSEL0,
			     D3323AA_FILTER_TYPE_NR_BITS);
	for (idx = 0; idx < ARRAY_SIZE(d3323aa_lp_filter_regval); ++idx) {
		if (d3323aa_lp_filter_regval[idx] == regval)
			break;
	}

	if (idx == ARRAY_SIZE(d3323aa_lp_filter_regval))
		return -EINVAL;

	*val = d3323aa_lp_filter_freq[idx][0];
	*val2 = d3323aa_lp_filter_freq[idx][1];

	return 0;
}

static int d3323aa_set_lp_filter_freq(unsigned long *regmap, const int val,
				      int val2)
{
	size_t idx;

	/* Truncate fractional part to one digit. */
	val2 /= 100000;

	for (idx = 0; idx < ARRAY_SIZE(d3323aa_lp_filter_freq); ++idx) {
		int integer = d3323aa_lp_filter_freq[idx][0] /
			      d3323aa_lp_filter_freq[idx][1];
		int fract = d3323aa_lp_filter_freq[idx][0] %
			    d3323aa_lp_filter_freq[idx][1];
		if (val == integer && val2 == fract)
			break;
	}

	if (idx == ARRAY_SIZE(d3323aa_lp_filter_freq))
		return -ERANGE;

	bitmap_write(regmap, d3323aa_lp_filter_regval[idx],
		     D3323AA_REG_BIT_FILSEL0, D3323AA_FILTER_TYPE_NR_BITS);

	return 0;
}

static int d3323aa_get_filter_gain(unsigned long *regmap, int *val)
{
	size_t idx;
	u8 regval;

	regval = bitmap_read(regmap, D3323AA_REG_BIT_FSTEP0,
			     D3323AA_FILTER_GAIN_REG_NR_BITS);
	for (idx = 0; idx < ARRAY_SIZE(d3323aa_filter_gain_regval); ++idx) {
		if (d3323aa_filter_gain_regval[idx] == regval)
			break;
	}

	if (idx == ARRAY_SIZE(d3323aa_filter_gain_regval))
		return -EINVAL;

	*val = d3323aa_filter_gain[idx];

	return 0;
}

static int d3323aa_set_filter_gain(unsigned long *regmap, const int val)
{
	size_t idx;

	for (idx = 0; idx < ARRAY_SIZE(d3323aa_filter_gain); ++idx) {
		if (d3323aa_filter_gain[idx] == val)
			break;
	}

	if (idx == ARRAY_SIZE(d3323aa_filter_gain))
		return -ERANGE;

	bitmap_write(regmap, d3323aa_filter_gain_regval[idx],
		     D3323AA_REG_BIT_FSTEP0, D3323AA_FILTER_GAIN_REG_NR_BITS);

	return 0;
}

static void d3323aa_get_threshold(unsigned long *regmap, int *val)
{
	*val = bitmap_read(regmap, D3323AA_REG_BIT_DETLVLABS0,
			   D3323AA_THRESH_REG_NR_BITS);
}

static int d3323aa_set_threshold(unsigned long *regmap, const int val)
{
	if (val > ((1 << D3323AA_THRESH_REG_NR_BITS) - 1))
		return -ERANGE;

	bitmap_write(regmap, val, D3323AA_REG_BIT_DETLVLABS0,
		     D3323AA_THRESH_REG_NR_BITS);

	return 0;
}

static int d3323aa_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_HIGH_PASS_FILTER_3DB_FREQUENCY:
		*vals = (int *)d3323aa_hp_filter_freq;
		*type = IIO_VAL_FRACTIONAL;
		*length = 2 * ARRAY_SIZE(d3323aa_hp_filter_freq);
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		*vals = (int *)d3323aa_lp_filter_freq;
		*type = IIO_VAL_FRACTIONAL;
		*length = 2 * ARRAY_SIZE(d3323aa_lp_filter_freq);
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		*vals = (int *)d3323aa_filter_gain;
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(d3323aa_filter_gain);
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int d3323aa_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct d3323aa_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->regmap_lock);

	switch (mask) {
	case IIO_CHAN_INFO_HIGH_PASS_FILTER_3DB_FREQUENCY:
		ret = d3323aa_get_hp_filter_freq(data->regmap, val, val2);
		if (ret)
			return ret;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		ret = d3323aa_get_lp_filter_freq(data->regmap, val, val2);
		if (ret)
			return ret;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		ret = d3323aa_get_filter_gain(data->regmap, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int d3323aa_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct d3323aa_data *data = iio_priv(indio_dev);
	DECLARE_BITMAP(regmap, D3323AA_REG_NR_BITS);
	int ret;

	guard(mutex)(&data->regmap_lock);

	bitmap_copy(regmap, data->regmap, D3323AA_REG_NR_BITS);

	switch (mask) {
	case IIO_CHAN_INFO_HIGH_PASS_FILTER_3DB_FREQUENCY:
		/*
		 * We only allow to set the low-pass cutoff frequency, since
		 * that is the only way to unambigously choose the type of
		 * band-pass filter. For example, both filter type B and C have
		 * 0.3 Hz as high-pass cutoff frequency (see
		 * d3323aa_hp_filter_freq).
		 */
		return -EINVAL;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		ret = d3323aa_set_lp_filter_freq(regmap, val, val2);
		if (ret)
			return ret;
		break;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		ret = d3323aa_set_filter_gain(regmap, val);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return d3323aa_setup(indio_dev, regmap);
}

static int d3323aa_read_event(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir,
			      enum iio_event_info info, int *val, int *val2)
{
	struct d3323aa_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->regmap_lock);

	switch (info) {
	case IIO_EV_INFO_VALUE:
		d3323aa_get_threshold(data->regmap, val);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int d3323aa_write_event(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info, int val, int val2)
{
	struct d3323aa_data *data = iio_priv(indio_dev);
	DECLARE_BITMAP(regmap, D3323AA_REG_NR_BITS);
	int ret;

	guard(mutex)(&data->regmap_lock);

	bitmap_copy(regmap, data->regmap, D3323AA_REG_NR_BITS);

	switch (info) {
	case IIO_EV_INFO_VALUE:
		ret = d3323aa_set_threshold(regmap, val);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return d3323aa_setup(indio_dev, regmap);
}

static const struct iio_info d3323aa_info = {
	.read_avail = d3323aa_read_avail,
	.read_raw = d3323aa_read_raw,
	.write_raw = d3323aa_write_raw,
	.read_event_value = d3323aa_read_event,
	.write_event_value = d3323aa_write_event,
};

static const struct iio_event_spec d3323aa_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
};

static const struct iio_chan_spec d3323aa_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate =
			BIT(IIO_CHAN_INFO_HIGH_PASS_FILTER_3DB_FREQUENCY) |
			BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.info_mask_separate_available =
			BIT(IIO_CHAN_INFO_HIGH_PASS_FILTER_3DB_FREQUENCY) |
			BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY) |
			BIT(IIO_CHAN_INFO_HARDWAREGAIN),
		.event_spec = d3323aa_event_spec,
		.num_event_specs = ARRAY_SIZE(d3323aa_event_spec),
	},
};

static int d3323aa_probe(struct platform_device *pdev)
{
	DECLARE_BITMAP(default_regmap, D3323AA_REG_NR_BITS);
	struct d3323aa_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*data));
	if (!indio_dev)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "Could not allocate iio device\n");

	data = iio_priv(indio_dev);
	data->dev = &pdev->dev;
	platform_set_drvdata(pdev, indio_dev);

	init_completion(&data->reset_completion);

	ret = devm_mutex_init(data->dev, &data->regmap_lock);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Could not initialize mutex (%d)\n", ret);

	/* Request GPIOs. */
	data->gpiod_vdd = devm_gpiod_get(data->dev, "vdd", GPIOD_OUT_LOW);
	if (IS_ERR(data->gpiod_vdd))
		return dev_err_probe(data->dev, PTR_ERR(data->gpiod_vdd),
				     "Could not get GPIO vdd (%ld)\n",
				     PTR_ERR(data->gpiod_vdd));

	data->gpiod_clk_vout =
		devm_gpiod_get(data->dev, "clk-vout", GPIOD_OUT_LOW);
	if (IS_ERR(data->gpiod_clk_vout))
		return dev_err_probe(data->dev, PTR_ERR(data->gpiod_clk_vout),
				     "Could not get GPIO clk-vout (%ld)\n",
				     PTR_ERR(data->gpiod_clk_vout));

	data->gpiod_data = devm_gpiod_get(data->dev, "data", GPIOD_OUT_LOW);
	if (IS_ERR(data->gpiod_data))
		return dev_err_probe(data->dev, PTR_ERR(data->gpiod_data),
				     "Could not get GPIO data (%ld)\n",
				     PTR_ERR(data->gpiod_data));

	ret = gpiod_to_irq(data->gpiod_clk_vout);
	if (ret < 0)
		return dev_err_probe(data->dev, ret, "Could not get IRQ (%d)\n",
				     ret);

	data->irq = ret;

	/* Do one setup with the default values. */
	bitmap_zero(default_regmap, D3323AA_REG_NR_BITS);
	d3323aa_set_threshold(default_regmap, D3323AA_THRESH_DEFAULT_VAL);
	d3323aa_set_filter_gain(default_regmap,
				D3323AA_FILTER_GAIN_DEFAULT_VAL);
	ret = d3323aa_setup(indio_dev, default_regmap);
	if (ret)
		return ret;

	indio_dev->info = &d3323aa_info;
	indio_dev->name = D3323AA_DRV_NAME;
	indio_dev->channels = d3323aa_channels;
	indio_dev->num_channels = ARRAY_SIZE(d3323aa_channels);

	ret = devm_iio_device_register(data->dev, indio_dev);
	if (ret)
		return dev_err_probe(data->dev, ret,
				     "Could not register iio device (%d)\n",
				     ret);

	return 0;
}

static void d3323aa_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct d3323aa_data *data = iio_priv(indio_dev);

	if (data->detecting)
		free_irq(data->irq, indio_dev);
}

static const struct of_device_id d3323aa_of_match[] = {
	{
		.compatible = "nicera,d3323aa",
	},
	{}
};
MODULE_DEVICE_TABLE(of, d3323aa_of_match);

static struct platform_driver d3323aa_driver = {
	.probe = d3323aa_probe,
	.remove = d3323aa_remove,
	.driver = {
		.name = D3323AA_DRV_NAME,
		.of_match_table = d3323aa_of_match,
	},
};
module_platform_driver(d3323aa_driver);

MODULE_AUTHOR("Waqar Hameed <waqar.hameed@axis.com>");
MODULE_DESCRIPTION("Nicera D3-323-AA PIR sensor driver");
MODULE_LICENSE("GPL");
