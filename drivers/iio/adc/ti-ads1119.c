// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments ADS1119 ADC driver.
 *
 * Copyright 2024 Toradex
 */

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define ADS1119_DRIVER	"ads1119"

#define ADS1119_CMD_RESET		0x06
#define ADS1119_CMD_POWERDOWN		0x02
#define ADS1119_CMD_START_SYNC		0x08
#define ADS1119_CMD_RDATA		0x10
#define ADS1119_CMD_RREG_CONFIG		0x20
#define ADS1119_CMD_RREG_STATUS		0x24
#define ADS1119_CMD_WREG		0x40

#define ADS1119_CMD_RREG(reg)		(0x20 | ((reg) << 2))

/* Config register */
#define ADS1119_REG_CONFIG	0x00
#define ADS1119_CONFIG_VREF(val)	(val)
#define ADS1119_CONFIG_VREF_MASK	BIT(0)
#define ADS1119_CONFIG_CONV_MODE(val)	((val) << 1)
#define ADS1119_CONFIG_CM_MASK		BIT(1)
#define ADS1119_CONFIG_DATARATE(val)	((val) << 2)
#define ADS1119_CONFIG_DR_MASK		GENMASK(3, 2)
#define ADS1119_CONFIG_GAIN(val)	((val) << 4)
#define ADS1119_CONFIG_GAIN_MASK	BIT(4)
#define ADS1119_CONFIG_MUX(val)		((val) << 5)
#define ADS1119_CONFIG_MUX_MASK		GENMASK(7, 5)

#define ADS1119_VREF_INTERNAL	0
#define ADS1119_VREF_EXTERNAL	1

#define ADS1119_CM_SINGLE	0
#define ADS1119_CM_CONTINUOUS	1

#define ADS1119_DR_20_SPS	0
#define ADS1119_DR_90_SPS	1
#define ADS1119_DR_330_SPS	2
#define ADS1119_DR_1000_SPS	3

#define ADS1119_GAIN_1	0
#define ADS1119_GAIN_4	1

#define ADS1119_MUX_AIN0_AIN1	0
#define ADS1119_MUX_AIN2_AIN3	1
#define ADS1119_MUX_AIN1_AIN2	2
#define ADS1119_MUX_AIN0	3
#define ADS1119_MUX_AIN1	4
#define ADS1119_MUX_AIN2	5
#define ADS1119_MUX_AIN3	6
#define ADS1119_MUX_SHORTED	7

/* Status register */
#define ADS1119_REG_STATUS	0x01
#define ADS1119_STATUS_DRDY_MASK	BIT(7)

#define ADS1119_DEFAULT_GAIN		1
#define ADS1119_DEFAULT_DATARATE	20

#define ADS1119_SUSPEND_DELAY		2000

/* Timeout based on the minimum sample rate of 20 SPS (50000us) */
#define ADS1119_MAX_DRDY_TIMEOUT	85000

/*
 * Static allocate buffer for scans assuming one channel sample (2 or 4 bytes)
 * + timestamp (8 bytes).
 */
#define ADS1119_SCAN_BUFFER_SIZE	12

struct ads1119_data {
	const struct iio_chan_spec *channels;
	const struct iio_info *info;
	const unsigned long *available_scan_masks;
	int num_channels;
	int vref_internal_uv;
};

struct ads1119_channel_config {
	int gain;
	int datarate;
};

struct ads1119_state {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct iio_trigger *trig;
	struct regmap *regmap;
	struct regulator *vref;
	const struct ads1119_data *data;
	struct completion completion;
	u8 scan_buf[ADS1119_SCAN_BUFFER_SIZE];
	struct ads1119_channel_config *channels_cfg;
	unsigned int num_bytes_sample;
};

static const unsigned long ads1119_available_scan_masks[] = {
	BIT(ADS1119_MUX_AIN0_AIN1),
	BIT(ADS1119_MUX_AIN2_AIN3),
	BIT(ADS1119_MUX_AIN1_AIN2),
	BIT(ADS1119_MUX_AIN0),
	BIT(ADS1119_MUX_AIN1),
	BIT(ADS1119_MUX_AIN2),
	BIT(ADS1119_MUX_AIN3),
	0
};

static int ads1119_cmd(struct ads1119_state *st, unsigned int cmd)
{
	dev_dbg(&st->client->dev, "send command: %#x\n", cmd);

	return i2c_smbus_write_byte(st->client, cmd);
}

static int ads1119_cmd_wreg(struct ads1119_state *st, unsigned int val)
{
	dev_dbg(&st->client->dev, "wreg: %#x\n", val);

	return i2c_smbus_write_byte_data(st->client, ADS1119_CMD_WREG, val);
}

static int ads1119_cmd_rreg(struct ads1119_state *st, unsigned int cmd,
			    unsigned int *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(st->client, cmd);
	if (ret < 0)
		return ret;

	dev_dbg(&st->client->dev, "rreg %#x: %#x\n", cmd, ret);

	*val = ret;

	return 0;
}

static int ads1119_cmd_rdata(struct ads1119_state *st, unsigned int *val)
{
	int ret;

	ret = i2c_smbus_read_word_swapped(st->client, ADS1119_CMD_RDATA);
	if (ret < 0)
		return ret;

	dev_dbg(&st->client->dev, "rdata: %#x\n", ret);

	*val = ret;

	return 0;
}

static bool ads1119_writeable_reg(struct device *dev, unsigned int reg)
{
	return (reg == ADS1119_REG_CONFIG);
}

static int ads1119_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	return ads1119_cmd_rreg(context, ADS1119_CMD_RREG(reg), val);
}

static int ads1119_reg_write(void *context, unsigned int reg, unsigned int val)
{
	return ads1119_cmd_wreg(context, val);
}

static const struct regmap_config ads1119_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_write = ads1119_reg_write,
	.reg_read = ads1119_reg_read,
	.max_register = ADS1119_REG_STATUS,
	.writeable_reg = ads1119_writeable_reg,
};

static int ads1119_reset(struct ads1119_state *st)
{
	if (!st->reset_gpio)
		return ads1119_cmd(st, ADS1119_CMD_RESET);

	gpiod_set_value_cansleep(st->reset_gpio, 1);
	udelay(1);
	gpiod_set_value_cansleep(st->reset_gpio, 0);
	udelay(1);

	return 0;
}

static int ads1119_set_mux(struct ads1119_state *st, unsigned int mux)
{
	return regmap_update_bits(st->regmap, ADS1119_REG_CONFIG,
				  ADS1119_CONFIG_MUX_MASK,
				  ADS1119_CONFIG_MUX(mux));
}

static int ads1119_set_gain(struct ads1119_state *st, int gain)
{
	switch (gain) {
	case 1:
		gain = ADS1119_GAIN_1;
		break;
	case 4:
		gain = ADS1119_GAIN_4;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(st->regmap, ADS1119_REG_CONFIG,
				  ADS1119_CONFIG_GAIN_MASK,
				  ADS1119_CONFIG_GAIN(gain));
}

static int ads1119_set_datarate(struct ads1119_state *st, int datarate)
{
	switch (datarate) {
	case 20:
		datarate = ADS1119_DR_20_SPS;
		break;
	case 90:
		datarate = ADS1119_DR_90_SPS;
		break;
	case 330:
		datarate = ADS1119_DR_330_SPS;
		break;
	case 1000:
		datarate = ADS1119_DR_1000_SPS;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(st->regmap, ADS1119_REG_CONFIG,
				  ADS1119_CONFIG_DR_MASK,
				  ADS1119_CONFIG_DATARATE(datarate));
}

static int ads1119_set_conv_mode(struct ads1119_state *st, bool continuous)
{
	unsigned int mode;

	if (continuous)
		mode = ADS1119_CM_CONTINUOUS;
	else
		mode = ADS1119_CM_SINGLE;

	return regmap_update_bits(st->regmap, ADS1119_REG_CONFIG,
				  ADS1119_CONFIG_CM_MASK,
				  ADS1119_CONFIG_CONV_MODE(mode));
}

static int ads1119_configure_channel(struct ads1119_state *st, unsigned int mux,
				     int gain, int datarate)
{
	int ret;

	ret = ads1119_set_mux(st, mux);
	if (ret)
		return ret;

	ret = ads1119_set_gain(st, gain);
	if (ret)
		return ret;

	ret = ads1119_set_datarate(st, datarate);
	if (ret)
		return ret;

	return 0;
}

static int ads1119_set_power(struct ads1119_state *st, bool on)
{
	struct device *dev = &st->client->dev;
	int ret;

	if (on) {
		ret = pm_runtime_resume_and_get(dev);
	} else {
		pm_runtime_mark_last_busy(dev);
		ret = pm_runtime_put_autosuspend(dev);
	}

	return ret;
}

static int ads1119_poll_data_ready(struct ads1119_state *st,
				   struct iio_chan_spec const *chan)
{
	unsigned int datarate = st->channels_cfg[chan->address].datarate;
	unsigned long wait_time;
	unsigned int status;

	/* Sleep between poll half of the data rate */
	wait_time = DIV_ROUND_CLOSEST(1000000, 2 * datarate);

	return regmap_read_poll_timeout(st->regmap,
					ADS1119_REG_STATUS,
					status,
					(status & ADS1119_STATUS_DRDY_MASK) == ADS1119_STATUS_DRDY_MASK,
					wait_time,
					ADS1119_MAX_DRDY_TIMEOUT);
}

static int ads1119_read_data(struct ads1119_state *st,
			     struct iio_chan_spec const *chan,
			     unsigned int *val)
{
	unsigned int timeout;
	int ret;

	timeout = msecs_to_jiffies(ADS1119_MAX_DRDY_TIMEOUT);

	if (!st->client->irq)
		ret = ads1119_poll_data_ready(st, chan);
	else if (!wait_for_completion_timeout(&st->completion, timeout))
		ret = -ETIMEDOUT;
	else
		ret = 0;

	if (!ret)
		ret = ads1119_cmd_rdata(st, val);

	return ret;
}

static int ads1119_single_conversion(struct ads1119_state *st,
				     struct iio_chan_spec const *chan,
				     int *val,
				     bool calib_offset)
{
	unsigned int mux = chan->address;
	unsigned int sample;
	int gain, datarate;
	int ret;

	gain = st->channels_cfg[mux].gain;
	datarate = st->channels_cfg[mux].datarate;

	if (calib_offset)
		mux = ADS1119_MUX_SHORTED;

	ret = ads1119_configure_channel(st, mux, gain, datarate);
	if (ret)
		return ret;

	ret = ads1119_set_power(st, true);
	if (ret)
		return ret;

	ret = ads1119_cmd(st, ADS1119_CMD_START_SYNC);
	if (ret)
		return ret;

	ret = ads1119_read_data(st, chan, &sample);
	if (ret)
		return ret;

	ads1119_set_power(st, false);

	*val = sign_extend32(sample, chan->scan_type.realbits - 1);

	return IIO_VAL_INT;
}

static int ads1119_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct ads1119_state *st = iio_priv(indio_dev);
	unsigned int index = chan->address;
	int ret;

	if (index >= ADS1119_MUX_SHORTED)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		iio_device_claim_direct_scoped(return -EBUSY, indio_dev)
			return ads1119_single_conversion(st, chan, val, false);
		unreachable();
	case IIO_CHAN_INFO_OFFSET:
		iio_device_claim_direct_scoped(return -EBUSY, indio_dev)
			return ads1119_single_conversion(st, chan, val, true);
		unreachable();
	case IIO_CHAN_INFO_SCALE:
		if (st->vref)
			ret = regulator_get_voltage(st->vref);
		else
			ret = st->data->vref_internal_uv;

		if (ret < 0)
			return ret;

		*val = ret / 1000;
		*val /= st->channels_cfg[index].gain;
		*val2 = chan->scan_type.realbits - 1;

		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = st->channels_cfg[index].datarate;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ads1119_debugfs_reg_access(struct iio_dev *indio_dev, unsigned int reg,
				      unsigned int writeval, unsigned int *readval)
{
	struct ads1119_state *st = iio_priv(indio_dev);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

#define ADS1119_V_DIFF_CHAN(_chan, _chan2, _addr) {		\
	.type = IIO_VOLTAGE,					\
	.differential = 1,					\
	.indexed = 1,						\
	.address = _addr,					\
	.channel = _chan,					\
	.channel2 = _chan2,					\
	.info_mask_separate =					\
		BIT(IIO_CHAN_INFO_RAW) |			\
		BIT(IIO_CHAN_INFO_SCALE) |			\
		BIT(IIO_CHAN_INFO_OFFSET) |			\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.scan_index = _addr,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 16,					\
		.storagebits = 16,				\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = "AIN"#_chan"-AIN"#_chan2,		\
}

#define ADS1119_V_CHAN(_chan, _addr) {				\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.address = _addr,					\
	.channel = _chan,					\
	.info_mask_separate =					\
		BIT(IIO_CHAN_INFO_RAW) |			\
		BIT(IIO_CHAN_INFO_SCALE) |			\
		BIT(IIO_CHAN_INFO_OFFSET) |			\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.scan_index = _addr,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 16,					\
		.storagebits = 16,				\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = "AIN"#_chan,				\
}

static const struct iio_chan_spec ads1119_channels[] = {
	ADS1119_V_DIFF_CHAN(0, 1, ADS1119_MUX_AIN0_AIN1),
	ADS1119_V_DIFF_CHAN(2, 3, ADS1119_MUX_AIN2_AIN3),
	ADS1119_V_DIFF_CHAN(1, 2, ADS1119_MUX_AIN1_AIN2),
	ADS1119_V_CHAN(0, ADS1119_MUX_AIN0),
	ADS1119_V_CHAN(1, ADS1119_MUX_AIN1),
	ADS1119_V_CHAN(2, ADS1119_MUX_AIN2),
	ADS1119_V_CHAN(3, ADS1119_MUX_AIN3),
	IIO_CHAN_SOFT_TIMESTAMP(ADS1119_MUX_SHORTED),
};

static const struct iio_info ads1119_info = {
	.read_raw = ads1119_read_raw,
	.debugfs_reg_access = ads1119_debugfs_reg_access,
};

static int ads1119_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct ads1119_state *st = iio_priv(indio_dev);
	unsigned int index;
	int ret;

	index = find_first_bit(indio_dev->active_scan_mask,
			       indio_dev->masklength);

	if (index >= ADS1119_MUX_SHORTED)
		return -EINVAL;

	st->num_bytes_sample =
		indio_dev->channels[index].scan_type.storagebits / 8;

	ret = ads1119_set_conv_mode(st, state);
	if (ret)
		return ret;

	if (state) {
		ret = ads1119_configure_channel(st, index,
						st->channels_cfg[index].gain,
						st->channels_cfg[index].datarate);
		if (ret)
			return ret;

		ret = ads1119_set_power(st, true);
		if (ret)
			return ret;

		ret = ads1119_cmd(st, ADS1119_CMD_START_SYNC);
	} else {
		ret = ads1119_set_power(st, false);
	}

	return ret;
}

static const struct iio_trigger_ops ads1119_trigger_ops = {
	.set_trigger_state = &ads1119_set_trigger_state,
	.validate_device = &iio_trigger_validate_own_device,
};

static irqreturn_t ads1119_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ads1119_state *st = iio_priv(indio_dev);

	if (iio_buffer_enabled(indio_dev))
		iio_trigger_poll(indio_dev->trig);
	else
		complete(&st->completion);

	return IRQ_HANDLED;
}

static irqreturn_t ads1119_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1119_state *st = iio_priv(indio_dev);
	unsigned int sample;
	int ret;

	ret = ads1119_cmd_rdata(st, &sample);
	if (!ret) {
		memcpy(st->scan_buf, &sample, st->num_bytes_sample);
		iio_push_to_buffers_with_timestamp(indio_dev, st->scan_buf,
						   iio_get_time_ns(indio_dev));
	} else {
		dev_err(&st->client->dev,
			"Failed to read data on trigger (%d)\n", ret);
	}

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ads1119_init(struct ads1119_state *st)
{
	int ret;

	ret = ads1119_reset(st);
	if (ret)
		return ret;

	if (st->vref)
		return regmap_update_bits(st->regmap, ADS1119_REG_CONFIG,
					  ADS1119_CONFIG_VREF_MASK,
					  ADS1119_CONFIG_VREF(ADS1119_VREF_EXTERNAL));
	return 0;
}

static int ads1119_alloc_and_config_channels(struct ads1119_state *st)
{
	struct device *dev = &st->client->dev;
	int num_channels, gain, datarate;
	struct fwnode_handle *node;
	int ret, index;

	/* Last channel is not a hw channel (timestamp) */
	num_channels = st->data->num_channels - 1;

	st->channels_cfg = devm_kcalloc(dev, num_channels,
					sizeof(*st->channels_cfg),
					GFP_KERNEL);
	if (!st->channels_cfg)
		return -ENOMEM;

	for (index = 0; index < num_channels; index++) {
		st->channels_cfg[index].gain = ADS1119_DEFAULT_GAIN;
		st->channels_cfg[index].datarate = ADS1119_DEFAULT_DATARATE;
	}

	device_for_each_child_node(dev, node) {
		ret = fwnode_property_read_u32(node, "reg", &index);
		if (ret) {
			fwnode_handle_put(node);
			return dev_err_probe(dev, ret,
					     "Failed to configure channels\n");
		}

		if (!fwnode_property_read_u32(node, "ti,gain", &gain))
			st->channels_cfg[index].gain = gain;

		if (!fwnode_property_read_u32(node, "ti,datarate", &datarate))
			st->channels_cfg[index].datarate = datarate;

		dev_dbg(dev, "channel config %d: gain %d, datarate %d\n",
			index, st->channels_cfg[index].gain,
			st->channels_cfg[index].datarate);
	}

	return 0;
}

static int ads1119_get_configs(struct ads1119_state *st)
{
	struct device *dev = &st->client->dev;

	st->vref = devm_regulator_get_optional(dev, "vref");
	if (IS_ERR(st->vref))
		return dev_err_probe(dev, PTR_ERR(st->vref),
				     "Failed to get vref\n");

	st->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(st->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(st->vref),
				     "Failed to get reset gpio\n");

	return ads1119_alloc_and_config_channels(st);
}

static void ads1119_disable_vref_regulator(void *data)
{
	struct ads1119_state *st = data;

	regulator_disable(st->vref);
}

static int ads1119_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct ads1119_state *st;
	struct device *dev = &client->dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate IIO device\n");

	st = iio_priv(indio_dev);
	st->client = client;
	st->data = i2c_get_match_data(client);
	if (!st->data)
		return dev_err_probe(dev, -ENODEV,
				     "Failed to get match data\n");

	indio_dev->name = ADS1119_DRIVER;
	indio_dev->info = st->data->info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = st->data->channels;
	indio_dev->num_channels = st->data->num_channels;
	indio_dev->available_scan_masks = st->data->available_scan_masks;

	i2c_set_clientdata(client, indio_dev);

	ret = ads1119_get_configs(st);
	if (ret)
		return ret;

	if (st->vref) {
		ret = regulator_enable(st->vref);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to enable vref\n");

		ret = devm_add_action_or_reset(dev, ads1119_disable_vref_regulator, st);
		if (ret)
			return ret;
	}

	st->regmap = devm_regmap_init(dev, NULL, st, &ads1119_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to allocate register map\n");

	init_completion(&st->completion);

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev,
						client->irq,
						ads1119_irq_handler,
						NULL,
						IRQF_TRIGGER_FALLING,
						ADS1119_DRIVER,
						indio_dev);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to allocate irq\n");

		st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
						  indio_dev->name,
						  iio_device_id(indio_dev));
		if (!st->trig)
			return dev_err_probe(dev, -ENOMEM,
					     "Failed to allocate IIO trigger\n");

		st->trig->ops = &ads1119_trigger_ops;
		iio_trigger_set_drvdata(st->trig, indio_dev);

		ret = devm_iio_trigger_register(dev, st->trig);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to register IIO trigger\n");

		indio_dev->trig = iio_trigger_get(st->trig);

		ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
						      ads1119_trigger_handler,
						      NULL);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to setup IIO buffer\n");
	}

	ret = ads1119_init(st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to initialize device\n");

	pm_runtime_set_autosuspend_delay(dev, ADS1119_SUSPEND_DELAY);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_set_active(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to enable pm runtime\n");

	return devm_iio_device_register(dev, indio_dev);
}

static int ads1119_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct ads1119_state *st = iio_priv(indio_dev);

	return ads1119_cmd(st, ADS1119_CMD_POWERDOWN);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ads1119_pm_ops,
				 ads1119_runtime_suspend,
				 NULL,
				 NULL);

static const struct ads1119_data ads1119_data = {
	.channels = ads1119_channels,
	.num_channels = ARRAY_SIZE(ads1119_channels),
	.info = &ads1119_info,
	.available_scan_masks = ads1119_available_scan_masks,
	.vref_internal_uv = 2048000,
};

static const struct of_device_id __maybe_unused ads1119_of_match[] = {
	{ .compatible = "ti,ads1119", .data = &ads1119_data, },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1119_of_match);

static const struct i2c_device_id ads1119_id[] = {
	{ ADS1119_DRIVER, (kernel_ulong_t)&ads1119_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ads1119_id);

static struct i2c_driver ads1119_driver = {
	.driver = {
		.name = ADS1119_DRIVER,
		.of_match_table = of_match_ptr(ads1119_of_match),
		.pm = pm_ptr(&ads1119_pm_ops),
	},
	.probe = ads1119_probe,
	.id_table = ads1119_id,
};
module_i2c_driver(ads1119_driver);

MODULE_AUTHOR("João Paulo Gonçalves <jpaulo.silvagoncalves@gmail.com>");
MODULE_DESCRIPTION("Texas Instruments ADS1119 ADC Driver");
MODULE_LICENSE("GPL");
