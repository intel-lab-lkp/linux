// SPDX-License-Identifier: GPL-2.0
/**
 * This file is part of the APDS9160 sensor driver.
 * Chip is combined proximity and ambient light sensor.
 * Author: Mikael Gonella-Bolduc <m.gonella.bolduc@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>

#define APDS9160_DRIVER_NAME "apds9160"
#define APDS9160_REGMAP_NAME "apds9160_regmap"
#define APDS9160_STARTUP_DELAY 25000 /* us */
#define APDS9160_REG_CNT 37

/** Main control register */
#define APDS9160_REG_CTRL 0x00
#define APDS9160_REG_CTRL_SWRESET BIT(4) /* 1: Activate reset */
#define APDS9160_REG_CTRL_MODE_RGB BIT(2) /* 0: ALS & IR, 1: RGB & IR */
#define APDS9160_REG_CTRL_EN_ALS BIT(1) /* 1: ALS active */
#define APDS9160_REG_CTLR_EN_PS BIT(0) /* 1: PS active */

/** Status register  */
#define APDS9160_REG_SR_LS_INT BIT(3)
#define APDS9160_REG_SR_LS_NEW_DATA BIT(2)
#define APDS9160_REG_SR_PS_INT BIT(1)
#define APDS9160_REG_SR_PS_NEW_DATA BIT(0)

/* Interrupt configuration register */
#define APDS9160_REG_INT_CFG 0x19
#define APDS9160_REG_INT_CFG_EN_LS BIT(2) /* LS int enable */
#define APDS9160_REG_INT_CFG_EN_PS BIT(0) /* PS int enable */
#define APDS9160_REG_INT_PST 0x1A

/* Proximity registers */
#define APDS9160_REG_PS_LED 0x01
#define APDS9160_REG_PS_PULSES 0x02
#define APDS9160_REG_PS_MEAS_RATE 0x03
#define APDS9160_REG_PS_THRES_HI_LSB 0x1B
#define APDS9160_REG_PS_THRES_HI_MSB 0x1C
#define APDS9160_REG_PS_THRES_LO_LSB 0x1D
#define APDS9160_REG_PS_THRES_LO_MSB 0x1E
#define APDS9160_REG_PS_DATA_LSB 0x08
#define APDS9160_REG_PS_DATA_MSB 0x09
#define APDS9160_REG_PS_CAN_LEVEL_DIG_LSB 0x1F
#define APDS9160_REG_PS_CAN_LEVEL_DIG_MSB 0x20
#define APDS9160_REG_PS_CAN_LEVEL_ANA_DUR 0x21
#define APDS9160_REG_PS_CAN_LEVEL_ANA_CURRENT 0x22

/* Light sensor registers */
#define APDS9160_REG_LS_MEAS_RATE 0x04
#define APDS9160_REG_LS_GAIN 0x05
#define APDS9160_REG_LS_DATA_CLEAR_LSB 0x0A
#define APDS9160_REG_LS_DATA_CLEAR 0x0B
#define APDS9160_REG_LS_DATA_CLEAR_MSB 0x0C
#define APDS9160_REG_LS_DATA_ALS_LSB 0x0D
#define APDS9160_REG_LS_DATA_ALS 0x0E
#define APDS9160_REG_LS_DATA_ALS_MSB 0x0F
#define APDS9160_REG_LS_THRES_UP_LSB 0x24
#define APDS9160_REG_LS_THRES_UP 0x25
#define APDS9160_REG_LS_THRES_UP_MSB 0x26
#define APDS9160_REG_LS_THRES_LO_LSB 0x27
#define APDS9160_REG_LS_THRES_LO 0x28
#define APDS9160_REG_LS_THRES_LO_MSB 0x29
#define APDS9160_REG_LS_THRES_VAR 0x2A

/** Part identification number register */
#define APDS9160_REG_ID 0x06

/** Status register */
#define APDS9160_REG_SR 0x07
#define APDS9160_REG_SR_DATA_ALS BIT(3)
#define APDS9160_REG_SR_DATA_PS BIT(0)

/* Supported ID:s */
#define APDS9160_PART_ID_0 0x00
#define APDS9160_PART_ID_MASK 0xF0
#define APDS9160_PART_REV_MASK 0x0F

#define APDS9160_PS_THRES_MAX 0x7FF
#define APDS9160_LS_THRES_MAX 0xFFFFF
#define APDS9160_CMD_LS_RESOLUTION_25MS 0x04
#define APDS9160_CMD_LS_RESOLUTION_50MS 0x03
#define APDS9160_CMD_LS_RESOLUTION_100MS 0x02
#define APDS9160_CMD_LS_RESOLUTION_200MS 0x01
#define APDS9160_PS_DATA_MASK 0x7FF

#define APDS9160_DEFAULT_LS_GAIN 3
#define APDS9160_DEFAULT_LS_RATE 100
#define APDS9160_DEFAULT_PS_RATE 100
#define APDS9160_DEFAULT_PS_CANCELLATION_LEVEL 0
#define APDS9160_DEFAULT_PS_ANALOG_CANCELLATION 0
#define APDS9160_DEFAULT_PS_GAIN 1
#define APDS9160_DEFAULT_PS_CURRENT 100
#define APDS9160_DEFAULT_PS_RESOLUTION 0x03 // 11 bits

// clang-format off
static const struct reg_default apds9160_reg_defaults[] = {
	{ APDS9160_REG_CTRL, 0x00 }, /* Sensors disabled by default  */
	{ APDS9160_REG_PS_LED, 0x33 }, /* 60 kHz frequency, 100 mA pulse current */
	{ APDS9160_REG_PS_PULSES, 0x08 }, /* 8 pulses */
	{ APDS9160_REG_PS_MEAS_RATE, 0x05 },
	{ APDS9160_REG_LS_MEAS_RATE, 0x22 },
	{ APDS9160_REG_LS_GAIN, 0x01 },
	{ APDS9160_REG_INT_CFG, 0x10 },
	{ APDS9160_REG_INT_PST, 0x00 },
	{ APDS9160_REG_PS_THRES_HI_LSB, 0xFF },
	{ APDS9160_REG_PS_THRES_HI_MSB, 0x07 },
	{ APDS9160_REG_PS_THRES_LO_LSB, 0x00 },
	{ APDS9160_REG_PS_THRES_LO_MSB, 0x00 },
	{ APDS9160_REG_PS_CAN_LEVEL_DIG_LSB, 0x00 },
	{ APDS9160_REG_PS_CAN_LEVEL_DIG_MSB, 0x00 },
	{ APDS9160_REG_PS_CAN_LEVEL_ANA_DUR, 0x00 },
	{ APDS9160_REG_PS_CAN_LEVEL_ANA_CURRENT, 0x00 },
	{ APDS9160_REG_LS_THRES_UP_LSB, 0xFF },
	{ APDS9160_REG_LS_THRES_UP, 0xFF },
	{ APDS9160_REG_LS_THRES_UP_MSB, 0x0F },
	{ APDS9160_REG_LS_THRES_LO_LSB, 0x00 },
	{ APDS9160_REG_LS_THRES_LO, 0x00 },
	{ APDS9160_REG_LS_THRES_LO_MSB, 0x00 },
	{ APDS9160_REG_LS_THRES_VAR, 0x00 },
};
// clang-format on

static const struct regmap_range apds9160_readable_ranges[] = {
	regmap_reg_range(APDS9160_REG_CTRL, APDS9160_REG_LS_THRES_VAR),
};

static const struct regmap_access_table apds9160_readable_table = {
	.yes_ranges = apds9160_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(apds9160_readable_ranges),
};

static const struct regmap_range apds9160_writeable_ranges[] = {
	regmap_reg_range(APDS9160_REG_CTRL, APDS9160_REG_LS_GAIN),
	regmap_reg_range(APDS9160_REG_INT_CFG, APDS9160_REG_LS_THRES_VAR),
};

static const struct regmap_access_table apds9160_writeable_table = {
	.yes_ranges = apds9160_writeable_ranges,
	.n_yes_ranges = ARRAY_SIZE(apds9160_writeable_ranges),
};

static const struct regmap_range apds9160_volatile_ranges[] = {
	regmap_reg_range(APDS9160_REG_SR, APDS9160_REG_LS_DATA_ALS_MSB),
};

static const struct regmap_access_table apds9160_volatile_table = {
	.yes_ranges = apds9160_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(apds9160_volatile_ranges),
};

static const struct regmap_config apds9160_regmap_config = {
	.name = APDS9160_REGMAP_NAME,
	.reg_bits = 8,
	.val_bits = 8,
	.use_single_read = true,
	.use_single_write = true,

	.rd_table = &apds9160_readable_table,
	.wr_table = &apds9160_writeable_table,
	.volatile_table = &apds9160_volatile_table,

	.reg_defaults = apds9160_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(apds9160_reg_defaults),
	.max_register = APDS9160_REG_CNT,
	.cache_type = REGCACHE_RBTREE,
};

static const struct iio_event_spec apds9160_ps_event_spec[] = {
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
	{
		.type = IIO_EV_TYPE_THRESH,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_event_spec apds9160_als_event_spec[] = {
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
	{
		.type = IIO_EV_TYPE_THRESH,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec apds9160_channels[] = {
	{
		/* Proximity sensor channel */
		.type = IIO_PROXIMITY,
		.address = APDS9160_REG_PS_DATA_LSB,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ) |
					    BIT(IIO_CHAN_INFO_CALIBSCALE) |
					    BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
					    BIT(IIO_CHAN_INFO_CALIBBIAS),
		.channel = 0,
		.indexed = 0,
		.scan_index = -1,

		.event_spec = apds9160_ps_event_spec,
		.num_event_specs = ARRAY_SIZE(apds9160_ps_event_spec),
	},
	{
		/* Proximity sensor led current */
		.type = IIO_CURRENT,
		.output = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = -1,
	},
	{
		/* Clear channel */
		.type = IIO_INTENSITY,
		.address = APDS9160_REG_LS_DATA_CLEAR_LSB,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_INT_TIME) |
					    BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
					    BIT(IIO_CHAN_INFO_SCALE),
		.channel2 = IIO_MOD_LIGHT_CLEAR,
		.modified = 1,
		.scan_index = -1,

		.event_spec = apds9160_als_event_spec,
		.num_event_specs = ARRAY_SIZE(apds9160_als_event_spec),
	},
	{
		/* Illuminance */
		.type = IIO_LIGHT,
		.address = APDS9160_REG_LS_DATA_ALS_LSB,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = -1,
	}
};

/* Attributes */
// clang-format off
static const int apds9160_als_rate_map[][2] = {
	{ 25, 0x00 },
	{ 50, 0x01 },
	{ 100, 0x02 },
	{ 200, 0x03 }
};

static const int apds9160_als_gain_map[][2] = {
	{ 1, 0x00 },
	{ 3, 0x01 },
	{ 6, 0x02 },
	{ 18, 0x03 },
	{ 64, 0x04 }
};

static const int apds9160_ps_gain_map[][2] = {
	{ 1, 0x00 },
	{ 2, 0x01},
	{ 4, 0x02},
	{ 8, 0x03}
};

static const int apds9160_ps_rate_map[][2] = {
	{ 25, 0x03 },
	{ 50, 0x04 },
	{ 100, 0x05 },
	{ 200, 0x06 },
	{ 400, 0x07 }
};

static const int adps9160_ps_led_current_map[][2] = {
	{ 10, 0x00 },
	{ 25, 0x01 },
	{ 50, 0x02 },
	{ 100, 0x03 },
	{ 150, 0x04},
	{ 175, 0x05 },
	{ 200, 0x06 }
};
// clang-format on

struct apds9160_scale {
	int itime;
	int gain;
	int scale1;
	int scale2;
};

static const struct apds9160_scale apds9160_als_scale_map[] = {
	{
		.gain = 1,
		.itime = 25,
		.scale1 = 3272,
		.scale2 = 1000,
	},
	{
		.gain = 1,
		.itime = 50,
		.scale1 = 1639,
		.scale2 = 1000,
	},
	{
		.gain = 1,
		.itime = 100,
		.scale1 = 819,
		.scale2 = 1000,
	},
	{
		.gain = 3,
		.itime = 25,
		.scale1 = 1077,
		.scale2 = 1000,
	},
	{
		.gain = 3,
		.itime = 50,
		.scale1 = 538,
		.scale2 = 1000,
	},
	{
		.gain = 3,
		.itime = 100,
		.scale1 = 269,
		.scale2 = 1000,
	},
	{
		.gain = 6,
		.itime = 25,
		.scale1 = 525,
		.scale2 = 1000,
	},
	{
		.gain = 6,
		.itime = 50,
		.scale1 = 263,
		.scale2 = 1000,
	},
	{
		.gain = 6,
		.itime = 100,
		.scale1 = 131,
		.scale2 = 1000,
	},
	{
		.gain = 18,
		.itime = 25,
		.scale1 = 169,
		.scale2 = 1000,
	},
	{
		.gain = 18,
		.itime = 50,
		.scale1 = 84,
		.scale2 = 1000,
	},
	{
		.gain = 18,
		.itime = 100,
		.scale1 = 42,
		.scale2 = 1000,
	},
	{
		.gain = 64,
		.itime = 25,
		.scale1 = 49,
		.scale2 = 1000,
	},
	{
		.gain = 64,
		.itime = 50,
		.scale1 = 25,
		.scale2 = 1000,
	},
	{
		.gain = 64,
		.itime = 100,
		.scale1 = 12,
		.scale2 = 1000,
	},
};

static IIO_CONST_ATTR(in_intensity_integration_time_available, "25 50 100 200");
static IIO_CONST_ATTR(in_proximity_sampling_frequency_available,
		      "25 50 100 200 400");
static IIO_CONST_ATTR(in_intensity_hardwaregain_available, "1 3 6 18 64");
static IIO_CONST_ATTR(in_proximity_hardwaregain_available, "1 2 4 8");
static IIO_CONST_ATTR(out_current_available, "10 25 50 100 150 175 200");

static struct attribute *apds9160_attributes[] = {
	&iio_const_attr_in_intensity_integration_time_available.dev_attr.attr,
	&iio_const_attr_in_intensity_hardwaregain_available.dev_attr.attr,
	&iio_const_attr_in_proximity_sampling_frequency_available.dev_attr.attr,
	&iio_const_attr_in_proximity_hardwaregain_available.dev_attr.attr,
	&iio_const_attr_out_current_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group apds9160_attribute_group = {
	.attrs = apds9160_attributes,
};

static const struct reg_field apds9160_reg_field_ls_en =
	REG_FIELD(APDS9160_REG_CTRL, 1, 1);

static const struct reg_field apds9160_reg_field_ps_en =
	REG_FIELD(APDS9160_REG_CTRL, 0, 0);

static const struct reg_field apds9160_reg_field_int_ps =
	REG_FIELD(APDS9160_REG_INT_CFG, 0, 0);

static const struct reg_field apds9160_reg_field_int_als =
	REG_FIELD(APDS9160_REG_INT_CFG, 2, 2);

static const struct reg_field apds9160_reg_field_ps_overflow =
	REG_FIELD(APDS9160_REG_PS_DATA_MSB, 3, 3);

static const struct reg_field apds9160_reg_field_als_rate =
	REG_FIELD(APDS9160_REG_LS_MEAS_RATE, 0, 2);

static const struct reg_field apds9160_reg_field_als_gain =
	REG_FIELD(APDS9160_REG_LS_GAIN, 0, 2);

static const struct reg_field apds9160_reg_field_ps_rate =
	REG_FIELD(APDS9160_REG_PS_MEAS_RATE, 0, 2);

static const struct reg_field apds9160_reg_field_als_res =
	REG_FIELD(APDS9160_REG_LS_MEAS_RATE, 4, 6);

static const struct reg_field apds9160_reg_field_ps_current =
	REG_FIELD(APDS9160_REG_PS_LED, 0, 2);

static const struct reg_field apds9160_reg_field_ps_gain =
	REG_FIELD(APDS9160_REG_PS_MEAS_RATE, 6, 7);

static const struct reg_field apds9160_reg_field_ps_resolution =
	REG_FIELD(APDS9160_REG_PS_MEAS_RATE, 3, 4);

struct apds9160_chip {
	struct i2c_client *client;
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	struct mutex lock; /* avoid parallel access */

	struct regmap_field *reg_enable_ps;
	struct regmap_field *reg_enable_als;
	struct regmap_field *reg_int_ps;
	struct regmap_field *reg_int_als;
	struct regmap_field *reg_ps_overflow;
	struct regmap_field *reg_als_rate;
	struct regmap_field *reg_als_resolution;
	struct regmap_field *reg_ps_rate;
	struct regmap_field *reg_als_gain;
	struct regmap_field *reg_ps_current;
	struct regmap_field *reg_ps_gain;
	struct regmap_field *reg_ps_resolution;

	/* State data */
	u8 revision;
	int als_int;
	int ps_int;

	/* Configuration values */
	int als_itime;
	int als_hwgain;
	int als_scale1;
	int als_scale2;
	int ps_rate;
	int ps_cancellation_level;
	int ps_cancellation_analog;
	int ps_current;
	int ps_gain;
};

static const struct i2c_device_id apds9160_id[] = { { APDS9160_DRIVER_NAME, 0 },
						    {} };

/** Called when mutex is locked */
static void apds9160_set_scale(struct apds9160_chip *data)
{
	for (int idx = 0; idx < ARRAY_SIZE(apds9160_als_scale_map); idx++) {
		if (data->als_hwgain == apds9160_als_scale_map[idx].gain &&
		    data->als_itime == apds9160_als_scale_map[idx].itime) {
			data->als_scale1 = apds9160_als_scale_map[idx].scale1;
			data->als_scale2 = apds9160_als_scale_map[idx].scale2;
		}
	}
}

static int apds9160_set_ps_rate(struct apds9160_chip *data, int val)
{
	int ret = -EINVAL;
	int idx;

	dev_dbg(&data->client->dev, "%s - set rate to %i\n", __func__, val);
	for (idx = 0; idx < ARRAY_SIZE(apds9160_ps_rate_map); idx++) {
		if (apds9160_ps_rate_map[idx][0] == val) {
			mutex_lock(&data->lock);
			ret = regmap_field_write(data->reg_ps_rate,
						 apds9160_ps_rate_map[idx][1]);
			if (!ret)
				data->ps_rate = val;
			mutex_unlock(&data->lock);
			break;
		}
	}

	return ret;
}

static int apds9160_set_ps_gain(struct apds9160_chip *data, int val)
{
	int ret = -EINVAL;
	int idx;

	dev_dbg(&data->client->dev, "%s - set gain to %i\n", __func__, val);
	for (idx = 0; idx < ARRAY_SIZE(apds9160_ps_gain_map); idx++) {
		if (apds9160_ps_gain_map[idx][0] == val) {
			mutex_lock(&data->lock);
			ret = regmap_field_write(data->reg_ps_gain,
						 apds9160_ps_gain_map[idx][1]);
			if (!ret)
				data->ps_gain = val;
			mutex_unlock(&data->lock);
			break;
		}
	}

	return ret;
}

/**
 * The PS intelligent cancellation level register allows for an on-chip substraction
 * of the ADC count caused by unwanted reflected light from PS ADC output.
 */
static int apds9160_set_ps_cancellation_level(struct apds9160_chip *data,
					      int val)
{
	int ret = -EINVAL;
	__le16 buf;

	dev_dbg(&data->client->dev, "%s - set cancellation level to %i\n",
		__func__, val);
	if (val < 0 || val > 0xFFFF)
		return ret;

	mutex_lock(&data->lock);
	buf = cpu_to_le16(val);
	ret = regmap_bulk_write(data->regmap, APDS9160_REG_PS_CAN_LEVEL_DIG_LSB,
				&buf, 2);
	if (!ret)
		data->ps_cancellation_level = val;
	mutex_unlock(&data->lock);
	return ret;
}

/**
 * This parameter determines the cancellation pulse duration in each of the PWM pulse.
 * The cancellation is applied during the integration phase of the PS measurement.
 * Duration is programmed in half clock cycles
 */
static int apds9160_set_ps_analog_cancellation(struct apds9160_chip *data,
					       int val)
{
	int ret = -EINVAL;

	dev_dbg(&data->client->dev, "%s - set analog cancellation to %i\n",
		__func__, val);
	if (val < 0 || val > 0x3F)
		return ret;

	mutex_lock(&data->lock);
	ret = regmap_write(data->regmap, APDS9160_REG_PS_CAN_LEVEL_ANA_DUR,
			   val);
	if (!ret)
		data->ps_cancellation_analog = val;
	mutex_unlock(&data->lock);
	return ret;
}

/**
 * Set the proximity sensor led current
 */
static int apds9160_set_ps_current(struct apds9160_chip *data, int val)
{
	int ret = -EINVAL;
	int idx;

	dev_dbg(&data->client->dev, "%s - set current to %i\n", __func__, val);
	for (idx = 0; idx < ARRAY_SIZE(adps9160_ps_led_current_map); idx++) {
		if (adps9160_ps_led_current_map[idx][0] == val) {
			mutex_lock(&data->lock);
			ret = regmap_field_write(
				data->reg_ps_current,
				adps9160_ps_led_current_map[idx][1]);

			if (!ret)
				data->ps_current = val;

			mutex_unlock(&data->lock);
			break;
		}
	}
	return ret;
}

static int apds9160_set_als_gain(struct apds9160_chip *data, int val)
{
	int ret = -EINVAL;
	int idx;

	dev_dbg(&data->client->dev, "%s - set als gain to %i\n", __func__, val);
	for (idx = 0; idx < ARRAY_SIZE(apds9160_als_gain_map); idx++) {
		if (apds9160_als_gain_map[idx][0] == val) {
			mutex_lock(&data->lock);
			ret = regmap_field_write(data->reg_als_gain,
						 apds9160_als_gain_map[idx][1]);
			if (!ret) {
				data->als_hwgain = val;
				apds9160_set_scale(data);
			}
			mutex_unlock(&data->lock);
			break;
		}
	}
	return ret;
}

static int apds9160_set_als_int_time(struct apds9160_chip *data, int val)
{
	int ret = -EINVAL;
	int idx;

	dev_dbg(&data->client->dev, "%s - set int time to %i\n", __func__, val);
	for (idx = 0; idx < ARRAY_SIZE(apds9160_als_rate_map); idx++) {
		if (apds9160_als_rate_map[idx][0] == val) {
			mutex_lock(&data->lock);
			ret = regmap_field_write(data->reg_als_rate,
						 apds9160_als_rate_map[idx][1]);
			if (!ret) {
				data->als_itime = val;
				/* Lower resolution for faster rates */
				switch (val) {
				case 25:
					ret = regmap_field_write(
						data->reg_als_resolution,
						APDS9160_CMD_LS_RESOLUTION_25MS);
					break;
				case 50:
					ret = regmap_field_write(
						data->reg_als_resolution,
						APDS9160_CMD_LS_RESOLUTION_50MS);
					break;
				case 200:
					ret = regmap_field_write(
						data->reg_als_resolution,
						APDS9160_CMD_LS_RESOLUTION_200MS);
					break;
				default:
					ret = regmap_field_write(
						data->reg_als_resolution,
						APDS9160_CMD_LS_RESOLUTION_100MS);
				}
				apds9160_set_scale(data);
			}
			mutex_unlock(&data->lock);
			break;
		}
	}

	return ret;
}

static int apds9160_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int *val,
			     int *val2, long mask)
{
	struct apds9160_chip *data = iio_priv(indio_dev);
	int ret = -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_PROXIMITY: {
			__le16 buf;

			ret = regmap_bulk_read(data->regmap, chan->address,
					       &buf, 2);
			if (!ret) {
				ret = IIO_VAL_INT;
				*val = le16_to_cpu(buf);
				// Remove data overflow and ambient light overflow bits from result
				*val &= APDS9160_PS_DATA_MASK;
			}
		} break;
		case IIO_LIGHT:
		case IIO_INTENSITY: {
			__le32 buf = 0;

			ret = regmap_bulk_read(data->regmap, chan->address,
					       &buf, 3);
			if (!ret) {
				ret = IIO_VAL_INT;
				*val = le32_to_cpu(buf);
			}
		} break;
		case IIO_CURRENT: {
			ret = IIO_VAL_INT;
			*val = data->ps_current;
		} break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		switch (chan->type) {
		case IIO_INTENSITY:
			ret = IIO_VAL_INT;
			*val = data->als_hwgain;
			break;
		case IIO_PROXIMITY:
			ret = IIO_VAL_INT;
			*val = data->ps_gain;
			break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_INT_TIME:
		switch (chan->type) {
		case IIO_INTENSITY:
			ret = IIO_VAL_INT;
			*val = data->als_itime;
			break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (chan->type) {
		case IIO_PROXIMITY:
			ret = IIO_VAL_INT;
			*val = data->ps_rate;
			break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_CALIBSCALE:
		switch (chan->type) {
		case IIO_PROXIMITY:
			ret = IIO_VAL_INT;
			*val = data->ps_cancellation_level;
			break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_CALIBBIAS:
		switch (chan->type) {
		case IIO_PROXIMITY:
			ret = IIO_VAL_INT;
			*val = data->ps_cancellation_analog;
			break;
		default:
			break;
		}
		break;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_LIGHT:
		case IIO_INTENSITY:
			ret = IIO_VAL_FRACTIONAL;
			*val = data->als_scale1;
			*val2 = data->als_scale2;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return ret;
};

static int apds9160_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int val,
			      int val2, long mask)
{
	struct apds9160_chip *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_INTENSITY:
			return apds9160_set_als_int_time(data, val);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_PROXIMITY:
			return apds9160_set_ps_rate(data, val);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_HARDWAREGAIN:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_INTENSITY:
			return apds9160_set_als_gain(data, val);
		case IIO_PROXIMITY:
			return apds9160_set_ps_gain(data, val);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_CALIBSCALE:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_PROXIMITY:
			return apds9160_set_ps_cancellation_level(data, val);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_CALIBBIAS:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_PROXIMITY:
			return apds9160_set_ps_analog_cancellation(data, val);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_RAW:
		if (val2 != 0)
			return -EINVAL;
		switch (chan->type) {
		case IIO_CURRENT:
			return apds9160_set_ps_current(data, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}

	return 0;
}

static inline int apds9160_get_thres_reg(const struct iio_chan_spec *chan,
					 enum iio_event_direction dir, u8 *reg)
{
	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (chan->type) {
		case IIO_PROXIMITY:
			*reg = APDS9160_REG_PS_THRES_HI_LSB;
			break;
		case IIO_INTENSITY:
			*reg = APDS9160_REG_LS_THRES_UP_LSB;
			break;
		default:
			return -EINVAL;
		}
		break;
	case IIO_EV_DIR_FALLING:
		switch (chan->type) {
		case IIO_PROXIMITY:
			*reg = APDS9160_REG_PS_THRES_LO_LSB;
			break;
		case IIO_INTENSITY:
			*reg = APDS9160_REG_LS_THRES_LO_LSB;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int apds9160_read_event(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info, int *val, int *val2)
{
	u8 reg;

	int ret = 0;
	struct apds9160_chip *data = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	ret = apds9160_get_thres_reg(chan, dir, &reg);
	if (ret < 0)
		return ret;

	if (chan->type == IIO_PROXIMITY) {
		__le16 buf;

		ret = regmap_bulk_read(data->regmap, reg, &buf, 2);
		if (ret < 0)
			return ret;
		*val = le16_to_cpu(buf);
	} else if (chan->type == IIO_INTENSITY) {
		__le32 buf = 0;

		ret = regmap_bulk_read(data->regmap, reg, &buf, 3);
		if (ret < 0)
			return ret;
		*val = le32_to_cpu(buf);
	} else
		return -EINVAL;

	*val2 = 0;

	return IIO_VAL_INT;
}

static int apds9160_write_event(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				enum iio_event_info info, int val, int val2)
{
	u8 reg;
	int ret = 0;
	struct apds9160_chip *data = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	ret = apds9160_get_thres_reg(chan, dir, &reg);
	if (ret < 0)
		return ret;

	if (chan->type == IIO_PROXIMITY) {
		if (val < 0 || val > APDS9160_PS_THRES_MAX)
			return -EINVAL;
		__le16 buf;

		buf = cpu_to_le16(val);
		ret = regmap_bulk_write(data->regmap, reg, &buf, 2);
		if (ret < 0)
			return ret;
	} else if (chan->type == IIO_INTENSITY) {
		if (val < 0 || val > APDS9160_LS_THRES_MAX)
			return -EINVAL;
		__le32 buf = 0;

		buf = cpu_to_le32(val);
		ret = regmap_bulk_write(data->regmap, reg, &buf, 3);
		if (ret < 0)
			return ret;
	} else
		return -EINVAL;

	return 0;
}

static int apds9160_read_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir)
{
	struct apds9160_chip *data = iio_priv(indio_dev);

	switch (chan->type) {
	case IIO_PROXIMITY:
		return data->ps_int;
	case IIO_INTENSITY:
		return data->als_int;
	default:
		return -EINVAL;
	}

	return 0;
}

static int apds9160_write_event_config(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       enum iio_event_type type,
				       enum iio_event_direction dir, int state)
{
	struct apds9160_chip *data = iio_priv(indio_dev);
	int ret;

	state = !!state;

	switch (chan->type) {
	case IIO_PROXIMITY:
		if (data->ps_int == state)
			return -EINVAL;

		ret = regmap_field_write(data->reg_int_ps, state);
		if (ret)
			return ret;
		data->ps_int = state;
		break;
	case IIO_INTENSITY:
		if (data->als_int == state)
			return -EINVAL;

		ret = regmap_field_write(data->reg_int_als, state);
		if (ret)
			return ret;
		data->als_int = state;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static irqreturn_t apds9160_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct apds9160_chip *data = iio_priv(indio_dev);
	int ret, status;

	/* Reading status register clears the interrupt flag */
	ret = regmap_read(data->regmap, APDS9160_REG_SR, &status);
	if (ret < 0) {
		dev_err(&data->client->dev, "irq status reg read failed\n");
		return IRQ_HANDLED;
	}

	if ((status & APDS9160_REG_SR_LS_INT) &&
	    (status & APDS9160_REG_SR_LS_NEW_DATA) && data->als_int) {
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_INTENSITY, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_EITHER),
			       iio_get_time_ns(indio_dev));
	}

	if ((status & APDS9160_REG_SR_PS_INT) &&
	    (status & APDS9160_REG_SR_PS_NEW_DATA) && data->ps_int) {
		/** Interrupt flag is cleared after data read */
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_EITHER),
			       iio_get_time_ns(indio_dev));
	}

	return IRQ_HANDLED;
}

static int apds9160_detect(struct apds9160_chip *chip)
{
	struct i2c_client *client = chip->client;
	int ret;
	u32 val;
	u8 id;
	u8 rev;

	ret = regmap_read(chip->regmap, APDS9160_REG_ID, &val);
	if (ret < 0) {
		dev_err(&client->dev, "ID read failed\n");
		return ret;
	}

	id = APDS9160_PART_ID_MASK & ((u8)val);
	rev = APDS9160_PART_REV_MASK & ((u8)val);

	switch (id) {
	case APDS9160_PART_ID_0:
		chip->revision = rev;
		dev_info(&client->dev, "Device probed, rev %u\n",
			 chip->revision);
		break;
	default:
		dev_info(&client->dev, "Unsupported part id %u rev %u\n", id,
			 rev);
		ret = -ENODEV;
		break;
	}
	return ret;
}

static int apds9160_chip_init(struct apds9160_chip *chip)
{
	int ret;

	/* Write default values to interrupt register */
	ret = regmap_field_write(chip->reg_int_ps, 0);
	chip->ps_int = 0;
	if (ret)
		return ret;

	ret = regmap_field_write(chip->reg_int_als, 0);
	chip->als_int = 0;
	if (ret)
		return ret;

	/* Write default values to control register */
	ret = regmap_field_write(chip->reg_enable_als, 1);
	if (ret)
		return ret;

	ret = regmap_field_write(chip->reg_enable_ps, 1);
	if (ret)
		return ret;

	/* Write other default values */
	ret = regmap_field_write(chip->reg_ps_resolution,
				 APDS9160_DEFAULT_PS_RESOLUTION);
	if (ret)
		return ret;

	/* Write default values to configuration registers */
	ret = apds9160_set_ps_current(chip, APDS9160_DEFAULT_PS_CURRENT);
	if (ret)
		return ret;

	apds9160_set_ps_rate(chip, APDS9160_DEFAULT_PS_RATE);
	if (ret)
		return ret;

	apds9160_set_als_int_time(chip, APDS9160_DEFAULT_LS_RATE);
	if (ret)
		return ret;

	ret = apds9160_set_als_gain(chip, APDS9160_DEFAULT_LS_GAIN);
	if (ret)
		return ret;

	ret = apds9160_set_ps_gain(chip, APDS9160_DEFAULT_PS_GAIN);
	if (ret)
		return ret;

	ret = apds9160_set_ps_analog_cancellation(
		chip, APDS9160_DEFAULT_PS_ANALOG_CANCELLATION);
	if (ret)
		return ret;

	return apds9160_set_ps_cancellation_level(
		chip, APDS9160_DEFAULT_PS_CANCELLATION_LEVEL);
}

static int apds9160_configure(struct iio_dev *indio_dev)
{
	struct apds9160_chip *chip = iio_priv(indio_dev);

	return apds9160_chip_init(chip);
}

static int apds9160_regfield_init(struct apds9160_chip *data)
{
	struct device *dev = &data->client->dev;
	struct regmap *regmap = data->regmap;

	data->reg_int_als = devm_regmap_field_alloc(dev, regmap,
						    apds9160_reg_field_int_als);
	if (IS_ERR(data->reg_int_als)) {
		dev_err(dev, "INT ALS reg field init failed\n");
		return PTR_ERR(data->reg_int_als);
	}

	data->reg_int_ps =
		devm_regmap_field_alloc(dev, regmap, apds9160_reg_field_int_ps);
	if (IS_ERR(data->reg_int_ps)) {
		dev_err(dev, "INT ps reg field init failed\n");
		return PTR_ERR(data->reg_int_ps);
	}

	data->reg_enable_als =
		devm_regmap_field_alloc(dev, regmap, apds9160_reg_field_ls_en);
	if (IS_ERR(data->reg_enable_als)) {
		dev_err(dev, "Enable ALS reg field init failed\n");
		return PTR_ERR(data->reg_enable_als);
	}

	data->reg_enable_ps =
		devm_regmap_field_alloc(dev, regmap, apds9160_reg_field_ps_en);
	if (IS_ERR(data->reg_enable_ps)) {
		dev_err(dev, "Enable PS reg field init failed\n");
		return PTR_ERR(data->reg_enable_ps);
	}

	data->reg_ps_overflow = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_ps_overflow);
	if (IS_ERR(data->reg_ps_overflow)) {
		dev_err(dev, "PS overflow reg field init failed\n");
		return PTR_ERR(data->reg_ps_overflow);
	}

	data->reg_als_rate = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_als_rate);
	if (IS_ERR(data->reg_als_rate)) {
		dev_err(dev, "ALS measurement rate field init failed\n");
		return PTR_ERR(data->reg_als_rate);
	}

	data->reg_als_resolution = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_als_res);
	if (IS_ERR(data->reg_als_resolution)) {
		dev_err(dev, "ALS resolution field init failed\n");
		return PTR_ERR(data->reg_als_resolution);
	}

	data->reg_ps_rate = devm_regmap_field_alloc(dev, regmap,
						    apds9160_reg_field_ps_rate);
	if (IS_ERR(data->reg_ps_rate)) {
		dev_err(dev, "PS measurement rate field init failed\n");
		return PTR_ERR(data->reg_ps_rate);
	}

	data->reg_als_gain = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_als_gain);
	if (IS_ERR(data->reg_als_gain)) {
		dev_err(dev, "ALS gain field init failed\n");
		return PTR_ERR(data->reg_als_gain);
	}

	data->reg_ps_current = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_ps_current);
	if (IS_ERR(data->reg_ps_current)) {
		dev_err(dev, "PS current field init failed\n");
		return PTR_ERR(data->reg_ps_current);
	}

	data->reg_ps_gain = devm_regmap_field_alloc(dev, regmap,
						    apds9160_reg_field_ps_gain);
	if (IS_ERR(data->reg_ps_gain)) {
		dev_err(dev, "PS gain field init failed\n");
		return PTR_ERR(data->reg_ps_gain);
	}

	data->reg_ps_resolution = devm_regmap_field_alloc(
		dev, regmap, apds9160_reg_field_ps_resolution);
	if (IS_ERR(data->reg_ps_resolution)) {
		dev_err(dev, "PS resolution field init failed\n");
		return PTR_ERR(data->reg_ps_resolution);
	}

	return 0;
}

static int apds9160_disable(struct apds9160_chip *data)
{
	int ret;

	ret = regmap_field_write(data->reg_enable_als, 0);
	if (ret)
		return ret;

	return regmap_field_write(data->reg_enable_ps, 0);
}

static int apds9160_shutdown(struct iio_dev *indio_dev)
{
	struct apds9160_chip *data = iio_priv(indio_dev);

	return apds9160_disable(data);
}

static void apds9160_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	struct apds9160_chip *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	apds9160_disable(data);
}

static const struct iio_buffer_setup_ops apds9160_buffer_setup_ops = {
	.postenable = apds9160_configure,
	.predisable = apds9160_shutdown,
};

static const struct iio_info apds9160_info = {
	.attrs = &apds9160_attribute_group,
	.read_raw = apds9160_read_raw,
	.write_raw = apds9160_write_raw,
	.read_event_value = apds9160_read_event,
	.write_event_value = apds9160_write_event,
	.read_event_config = apds9160_read_event_config,
	.write_event_config = apds9160_write_event_config,
};

static int apds9160_probe(struct i2c_client *client)
{
	struct apds9160_chip *chip;
	struct iio_dev *indio_dev;
	int err;

	dev_info(&client->dev,
		 "Loading proximity/ambient light sensor driver\n");
	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	indio_dev->info = &apds9160_info;
	indio_dev->name = APDS9160_DRIVER_NAME;
	indio_dev->channels = apds9160_channels;
	indio_dev->num_channels = ARRAY_SIZE(apds9160_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	err = devm_iio_kfifo_buffer_setup(&client->dev, indio_dev,
					  &apds9160_buffer_setup_ops);
	if (err)
		return err;

	chip = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	chip->client = client;
	chip->regmap = devm_regmap_init_i2c(client, &apds9160_regmap_config);
	if (IS_ERR(chip->regmap)) {
		dev_err(&client->dev, "regmap initialization failed.\n");
		return PTR_ERR(chip->regmap);
	}

	chip->client = client;
	chip->indio_dev = indio_dev;
	mutex_init(&chip->lock);

	err = apds9160_detect(chip);
	if (err < 0) {
		dev_err(&client->dev, "apds9160 not found\n");
		return err;
	}

	err = apds9160_regfield_init(chip);
	if (err)
		return err;

	err = apds9160_chip_init(chip);
	if (err)
		return err;

	if (client->irq > 0) {
		err = devm_request_threaded_irq(
			&client->dev, client->irq, NULL, apds9160_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "apds9160_event",
			indio_dev);
		if (err) {
			dev_err(&client->dev, "request irq (%d) failed\n",
				client->irq);
			goto fail;
		}
	} else {
		dev_info(&client->dev,
			 "init: no IRQ defined, ps/als interrupts disabled\n");
	}

	err = iio_device_register(indio_dev);
	if (err)
		goto fail;

	return 0;
fail:
	apds9160_disable(chip);
	return err;
}

static const struct of_device_id apds9160_of_match[] = {
	{ .compatible = "avago,apds9160" },
	{ .compatible = "broadmobi,apds9160" },
	{}
};
MODULE_DEVICE_TABLE(of, apds9160_of_match);

static struct i2c_driver apds9160_driver = {
	.driver	  = {
		.name	= APDS9160_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = apds9160_of_match,
	},
	.probe    = apds9160_probe,
	.remove	  = apds9160_remove,
	.id_table = apds9160_id,
};

MODULE_DEVICE_TABLE(i2c, apds9160_id);
module_i2c_driver(apds9160_driver);
MODULE_DESCRIPTION("APDS9160 combined ALS and proximity sensor");
MODULE_AUTHOR("Mikael Gonella-Bolduc <m.gonella.bolduc@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
