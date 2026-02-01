// SPDX-License-Identifier: GPL-2.0-only

#include <asm-generic/errno-base.h>
#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/iio/types.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/interrupt.h>

/* Device registers */
#define CM36686_REG_ALS_CONF		0x00
#define CM36686_REG_PS_CONF1		0x03
#define CM36686_REG_PS_CONF3		0x04
#define CM36686_REG_PS_THDL		0x06
#define CM36686_REG_PS_THDH		0x07
#define CM36686_REG_PS_DATA		0x08
#define CM36686_REG_ALS_DATA		0x09
#define CM36686_REG_INT_FLAG		0x0B
#define CM36686_REG_ID_FLAG		0x0C

/* ALS_CONF */
#define CM36686_ALS_IT			GENMASK(7, 6)
#define CM36686_ALS_GAIN		GENMASK(3, 2)
#define CM36686_ALS_INT_EN		BIT(1)
#define CM36686_ALS_SD			BIT(0)

/* PS_CONF1 */
#define CM36686_PS_DR			GENMASK(7, 6)
#define CM36686_PS_PERS			GENMASK(5, 4)
#define CM36686_PS_IT			GENMASK(3, 1)
#define CM36686_PS_SD			BIT(0)

#define CM36686_PS_INT_OUT		BIT(9)
#define CM36686_PS_INT_IN		BIT(8)

/* PS_CONF3 */
#define CM36686_PS_SMART_PERS_ENABLE	BIT(4)

#define CM36686_LED_I			GENMASK(10, 8)

/* INT_FLAG */
#define CM36686_PS_IF			GENMASK(9, 8)

/* Default values */
#define CM36686_ALS_ENABLE		0x00
#define CM36686_PS_DR_1_320		FIELD_PREP(CM36686_PS_DR, 3)
#define CM36686_PS_PERS_2		FIELD_PREP(CM36686_PS_PERS, 1)
#define CM36686_PS_IT_2_5T		FIELD_PREP(CM36686_PS_IT, 3)
#define CM36686_LED_I_100		FIELD_PREP(CM36686_LED_I, 2)

/* Shifts */
#define CM36686_INT_FLAG_SHIFT		8

/* Max proximity thresholds */
#define CM36686_MAX_PS_VALUE		(BIT(12) - 1)

#define CM36686_DEVICE_ID		0x86

enum {
	CM36686,
	CM36672P,
};

enum cm36686_distance {
	CM36686_AWAY = 1,
	CM36686_CLOSE,
	CM36686_BOTH
};

enum {
	CM36686_PS_CONF1,
	CM36686_PS_CONF3,
	CM36686_PS_CONF_NUM
};

enum {
	CM36686_SUPPLY_VDD,
	CM36686_SUPPLY_VDDIO,
	CM36686_SUPPLY_VLED,
	CM36686_SUPPLY_NUM,
};

static const int cm36686_als_it_times[][2] = {
	{0, 80000},
	{0, 160000},
	{0, 320000},
	{0, 640000}
};

static const int cm36686_ps_it_times[][2] = {
	{0, 320},
	{0, 480},
	{0, 640},
	{0, 800},
	{0, 960},
	{0, 1120},
	{0, 1280},
	{0, 2560}
};

static const int cm36686_ps_led_current[] = {
	50,
	75,
	100,
	120,
	140,
	160,
	180,
	200
};

struct cm36686_data {
	struct mutex lock;
	struct i2c_client *client;
	struct regulator_bulk_data supplies[CM36686_SUPPLY_NUM];
	int als_conf;
	int ps_conf[CM36686_PS_CONF_NUM];
	int ps_close;
	int ps_away;
};

struct cm36686_chip_info {
	const char *name;
	const struct iio_chan_spec *channels;
	const int num_channels;
};

static int cm36686_current_to_index(int led_current)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cm36686_ps_led_current); i++)
		if (led_current < cm36686_ps_led_current[i])
			break;

	return i > 0 ? i - 1 : -EINVAL;
}

static ssize_t cm36686_read_near_level(struct iio_dev *indio_dev,
				       uintptr_t priv,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	return sysfs_emit(buf, "%u\n", chip->ps_close);
}

static const struct iio_chan_spec_ext_info cm36686_ext_info[] = {
	{
		.name = "nearlevel",
		.shared = IIO_SEPARATE,
		.read = cm36686_read_near_level,
	},
	{}
};

static const struct iio_event_spec cm36686_proximity_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	}
};

static const struct iio_chan_spec cm36686_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_ALS_DATA,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.event_spec = cm36686_proximity_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36686_proximity_event_spec),
		.ext_info = cm36686_ext_info
	}
};

static const struct iio_chan_spec cm36672p_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_INT_TIME),
		.address = CM36686_REG_PS_DATA,
		.event_spec = cm36686_proximity_event_spec,
		.num_event_specs = ARRAY_SIZE(cm36686_proximity_event_spec),
		.ext_info = cm36686_ext_info
	}
};

static int cm36686_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	if (mask != IIO_CHAN_INFO_INT_TIME)
		return -EINVAL;

	switch (chan->type) {
	case IIO_LIGHT:
		*vals = (int *)(cm36686_als_it_times);
		*length = 2 * ARRAY_SIZE(cm36686_als_it_times);
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	case IIO_PROXIMITY:
		*vals = (int *)(cm36686_ps_it_times);
		*length = 2 * ARRAY_SIZE(cm36686_ps_it_times);
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int cm36686_read_channel(struct cm36686_data *chip,
				struct iio_chan_spec const *chan, int *val)
{
	struct i2c_client *client = chip->client;
	int ret = IIO_VAL_INT;

	int data = i2c_smbus_read_word_data(client, chan->address);

	if (data < 0) {
		dev_err(&client->dev, "Failed to read register: %pe", ERR_PTR(data));
		ret = -EIO;
	} else {
		*val = data;
	}
	return ret;
}

static int cm36686_read_int_time(struct cm36686_data *chip,
				 struct iio_chan_spec const *chan, int *val,
				 int *val2)
{
	int als_it_index, ps_it_index;

	switch (chan->type) {
	case IIO_LIGHT:
		als_it_index = FIELD_GET(CM36686_ALS_IT, chip->als_conf);
		*val = cm36686_als_it_times[als_it_index][0];
		*val2 = cm36686_als_it_times[als_it_index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_PROXIMITY:
		ps_it_index = FIELD_GET(CM36686_PS_IT,
			chip->ps_conf[CM36686_PS_CONF1]);
		*val = cm36686_ps_it_times[ps_it_index][0];
		*val2 = cm36686_ps_it_times[ps_it_index][1];
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int cm36686_write_light_int_time(struct cm36686_data *chip, int val2)
{
	struct i2c_client *client = chip->client;
	int index = -1, ret, new_it_time;

	for (int i = 0; i < ARRAY_SIZE(cm36686_als_it_times); i++) {
		if (cm36686_als_it_times[i][1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	new_it_time = chip->als_conf & ~CM36686_ALS_IT;
	new_it_time |= FIELD_PREP(CM36686_ALS_IT, index);

	ret = i2c_smbus_write_word_data(chip->client, CM36686_REG_ALS_CONF,
					new_it_time);
	if (ret < 0)
		dev_err(&client->dev,
			"Failed to set ALS integration time: %pe", ERR_PTR(ret));
	else
		chip->als_conf = new_it_time;

	return ret;
}

static int cm36686_write_prox_int_time(struct cm36686_data *chip, int val2)
{
	struct i2c_client *client = chip->client;
	int index = -1, ret, new_it_time;

	for (int i = 0; i < ARRAY_SIZE(cm36686_ps_it_times); i++) {
		if (cm36686_ps_it_times[i][1] == val2) {
			index = i;
			break;
		}
	}

	if (index == -1)
		return -EINVAL;

	new_it_time = chip->ps_conf[CM36686_PS_CONF1] & ~CM36686_PS_IT;
	new_it_time |= FIELD_PREP(CM36686_PS_IT, index);

	ret = i2c_smbus_write_word_data(chip->client, CM36686_REG_PS_CONF1,
					new_it_time);
	if (ret < 0)
		dev_err(&client->dev, "Failed to set PS integration time: %pe",
			ERR_PTR(ret));
	else
		chip->ps_conf[CM36686_PS_CONF1] = new_it_time;

	return ret;
}

static int cm36686_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	int ret;

	mutex_lock(&chip->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = cm36686_read_channel(chip, chan, val);
		break;
	case IIO_CHAN_INFO_INT_TIME:
		ret = cm36686_read_int_time(chip, chan, val, val2);
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&chip->lock);
	return ret;
}

static int cm36686_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	int ret;

	if (val) /* Integration time more than 1s is not supported */
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_INT_TIME)
		return -EINVAL;

	mutex_lock(&chip->lock);

	switch (chan->type) {
	case IIO_LIGHT:
		ret = cm36686_write_light_int_time(chip, val2);
		break;
	case IIO_PROXIMITY:
		ret = cm36686_write_prox_int_time(chip, val2);
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&chip->lock);
	return ret;
}

static int cm36686_read_prox_thresh(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int *val,
				    int *val2)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		*val = chip->ps_close;
		break;
	case IIO_EV_DIR_FALLING:
		*val = chip->ps_away;
		break;
	default:
		return -EINVAL;
	}

	return IIO_VAL_INT;
}

static int cm36686_write_prox_thresh(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int val,
				     int val2)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ret = 0, address, *thresh;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		if (val > chip->ps_close || val < 0)
			return -EINVAL;

		address = CM36686_REG_PS_THDL;
		thresh = &chip->ps_away;
		break;
	case IIO_EV_DIR_RISING:
		if (val < chip->ps_away || val > CM36686_MAX_PS_VALUE)
			return -EINVAL;

		address = CM36686_REG_PS_THDH;
		thresh = &chip->ps_close;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&chip->lock);

	ret = i2c_smbus_write_word_data(client, address, val);
	if (ret < 0)
		dev_err(&client->dev,
			"Failed to set PS threshold value: %pe", ERR_PTR(ret));
	else
		*thresh = val;

	mutex_unlock(&chip->lock);
	return ret;
}

static int cm36686_read_prox_event_config(struct iio_dev *indio_dev,
					  const struct iio_chan_spec *chan,
					  enum iio_event_type type,
					  enum iio_event_direction dir)
{
	struct cm36686_data *chip = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		return FIELD_GET(CM36686_PS_INT_OUT, chip->ps_conf[CM36686_PS_CONF1]);
	case IIO_EV_DIR_RISING:
		return FIELD_GET(CM36686_PS_INT_IN, chip->ps_conf[CM36686_PS_CONF1]);
	default:
		return -EINVAL;
	}
}

static int cm36686_write_prox_event_config(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan,
					   enum iio_event_type type,
					   enum iio_event_direction dir,
					   bool state)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ret = 0, new_ps_conf;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		new_ps_conf = chip->ps_conf[CM36686_PS_CONF1] & ~CM36686_PS_INT_OUT;
		new_ps_conf |= FIELD_PREP(CM36686_PS_INT_OUT, state);
		break;
	case IIO_EV_DIR_RISING:
		new_ps_conf = chip->ps_conf[CM36686_PS_CONF1] & ~CM36686_PS_INT_IN;
		new_ps_conf |= FIELD_PREP(CM36686_PS_INT_IN, state);
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&chip->lock);

	ret = i2c_smbus_write_word_data(chip->client, CM36686_REG_PS_CONF1, new_ps_conf);
	if (ret < 0)
		dev_err(&client->dev,
			"Failed to set proximity event interrupt config: %pe", ERR_PTR(ret));
	else
		chip->ps_conf[CM36686_PS_CONF1] = new_ps_conf;

	mutex_unlock(&chip->lock);

	return ret;
}

static int cm36686_fallback_read_ps(struct iio_dev *indio_dev)
{
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int data = i2c_smbus_read_word_data(client, CM36686_REG_PS_DATA);

	if (data < 0)
		return data;

	if (data < chip->ps_away)
		return IIO_EV_DIR_FALLING;
	else if (data > chip->ps_close)
		return IIO_EV_DIR_RISING;
	else
		return IIO_EV_DIR_EITHER;
}

static irqreturn_t cm36686_irq_handler(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct cm36686_data *chip = iio_priv(indio_dev);
	struct i2c_client *client = chip->client;
	int ev_dir, ret;
	u64 ev_code;

	/* Reading the interrupt flag acknowledges the interrupt */
	ret = i2c_smbus_read_word_data(client, CM36686_REG_INT_FLAG);
	if (ret < 0) {
		dev_err(&client->dev,
			"Interrupt flag register read failed: %pe", ERR_PTR(ret));
		return IRQ_HANDLED;
	}

	ret >>= CM36686_INT_FLAG_SHIFT;
	switch (ret) {
	case CM36686_CLOSE:
		ev_dir = IIO_EV_DIR_RISING;
		break;
	case CM36686_AWAY:
		ev_dir = IIO_EV_DIR_FALLING;
		break;
	case CM36686_BOTH:
		ev_dir = cm36686_fallback_read_ps(indio_dev);
		if (ev_dir < 0) {
			dev_err(&client->dev, "Failed to settle interrupt state: %pe",
				ERR_PTR(ret));
			return IRQ_HANDLED;
		}
		break;
	default:
		dev_err(&client->dev, "Unknown interrupt state: %x", ret);
		return IRQ_HANDLED;
	}
	ev_code = IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, IIO_EV_INFO_VALUE,
				       IIO_EV_TYPE_THRESH, ev_dir);

	iio_push_event(indio_dev, ev_code, iio_get_time_ns(indio_dev));
	return IRQ_HANDLED;
}

static const struct iio_info cm36686_info = {
	.read_avail =		cm36686_read_avail,
	.read_raw =		cm36686_read_raw,
	.write_raw =		cm36686_write_raw,
	.read_event_value =	cm36686_read_prox_thresh,
	.write_event_value =	cm36686_write_prox_thresh,
	.read_event_config =	cm36686_read_prox_event_config,
	.write_event_config =	cm36686_write_prox_event_config,
};

static struct cm36686_chip_info cm36686_chip_info_tbl[] = {
	[CM36686] = {
		.name = "cm36686",
		.channels = cm36686_channels,
		.num_channels = ARRAY_SIZE(cm36686_channels),
	},
	[CM36672P] = {
		.name = "cm36672p",
		.channels = cm36672p_channels,
		.num_channels = ARRAY_SIZE(cm36672p_channels),
	},
};

static int cm36686_setup(struct cm36686_data *chip)
{
	struct i2c_client *client = chip->client;
	int ret, led_current, led_index;

	chip->als_conf = CM36686_ALS_ENABLE;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_ALS_CONF,
					chip->als_conf);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to enable ambient light sensor: %pe", ERR_PTR(ret));
		return ret;
	}

	chip->ps_conf[CM36686_PS_CONF1] = CM36686_PS_INT_IN |
		CM36686_PS_INT_OUT | CM36686_PS_DR_1_320 |
		CM36686_PS_PERS_2 | CM36686_PS_IT_2_5T;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF1,
					chip->ps_conf[CM36686_PS_CONF1]);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to enable proximity sensor: %pe", ERR_PTR(ret));
		return ret;
	}

	chip->ps_conf[CM36686_PS_CONF3] = CM36686_PS_SMART_PERS_ENABLE;

	ret = device_property_read_u32(&client->dev,
		"capella,proximity-led-current", &led_current);
	if (!ret) {
		led_index = cm36686_current_to_index(led_current);
		if (led_index < 0) {
			dev_err(&client->dev, "No appropriate current for IR LED found.");
			return led_index;
		}

		chip->ps_conf[CM36686_PS_CONF3] &= ~CM36686_LED_I;
		chip->ps_conf[CM36686_PS_CONF3] |= FIELD_PREP(CM36686_LED_I, led_index);
	}

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF3,
					chip->ps_conf[CM36686_PS_CONF3]);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to enable proximity sensor: %pe", ERR_PTR(ret));
		return ret;
	}

	ret = device_property_read_u32(&client->dev, "proximity-near-level",
					    &chip->ps_close);
	if (ret < 0)
		chip->ps_close = 0;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDH,
		chip->ps_close);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to set close proximity threshold: %pe", ERR_PTR(ret));
		return ret;
	}

	chip->ps_away = chip->ps_close;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_THDL,
		chip->ps_away);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to set away proximity threshold: %pe", ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static void cm36686_shutdown(void *data)
{
	struct cm36686_data *chip = data;
	struct i2c_client *client = chip->client;
	int ret, als_shutdown, ps_shutdown;

	als_shutdown = chip->als_conf | CM36686_ALS_SD;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_ALS_CONF,
					als_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown ALS");

	ps_shutdown = chip->ps_conf[CM36686_PS_CONF1] | CM36686_PS_SD;

	ret = i2c_smbus_write_word_data(client, CM36686_REG_PS_CONF1,
					ps_shutdown);
	if (ret < 0)
		dev_err(&client->dev, "Failed to shutdown PS");

	ret = regulator_bulk_disable(ARRAY_SIZE(chip->supplies), chip->supplies);
	if (ret < 0)
		dev_err(&client->dev, "Failed to disable regulators");
}

static int cm36686_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct cm36686_data *chip;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev,
					  sizeof(struct cm36686_data));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);

	const struct i2c_device_id *id = i2c_client_get_device_id(client);

	const struct cm36686_chip_info *info =
		&cm36686_chip_info_tbl[id->driver_data];

	ret = i2c_smbus_read_byte_data(client, CM36686_REG_ID_FLAG);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "Failed to read device ID");

	if (ret != CM36686_DEVICE_ID)
		return dev_err_probe(&client->dev, -ENODEV, "Device not recognized!");

	i2c_set_clientdata(client, indio_dev);
	chip->client = client;
	ret = devm_mutex_init(&client->dev, &chip->lock);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
		       "Failed to initialize mutex");

	chip->supplies[CM36686_SUPPLY_VDD].supply = "vdd";
	chip->supplies[CM36686_SUPPLY_VDDIO].supply = "vddio";
	chip->supplies[CM36686_SUPPLY_VLED].supply = "vled";

	ret = devm_regulator_bulk_get(&client->dev,
		ARRAY_SIZE(chip->supplies), chip->supplies);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable regulators");

	ret = devm_add_action_or_reset(&client->dev, cm36686_shutdown, chip);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to set shutdown action");

	ret = cm36686_setup(chip);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to set up registers");

	indio_dev->name = info->name;
	indio_dev->channels = info->channels;
	indio_dev->num_channels = info->num_channels;
	indio_dev->info = &cm36686_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					cm36686_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					indio_dev->name, indio_dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to request irq");

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register iio device");

	return 0;
}

static const struct i2c_device_id cm36686_id[] = {
	{ "cm36686", CM36686 },
	{ "cm36672p", CM36672P },
	{}
};

MODULE_DEVICE_TABLE(i2c, cm36686_id);

static const struct of_device_id cm36686_of_match[] = {
	{ .compatible = "capella,cm36686" },
	{ .compatible = "capella,cm36672p" },
	{}
};
MODULE_DEVICE_TABLE(of, cm36686_of_match);

static struct i2c_driver cm36686_driver = {
	.driver = {
		.name = "cm36686",
		.of_match_table = cm36686_of_match,
	},
	.probe = cm36686_probe,
	.id_table = cm36686_id
};

module_i2c_driver(cm36686_driver);

MODULE_AUTHOR("Erikas Bitovtas <xerikasxx@gmail.com>");
MODULE_DESCRIPTION("CM36686 ambient light and proximity sensor driver");
MODULE_LICENSE("GPL");
