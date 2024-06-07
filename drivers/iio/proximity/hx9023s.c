// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 NanjingTianyihexin Electronics Ltd.
 * http://www.tianyihexin.com
 *
 * Driver for NanjingTianyihexin HX9023S Cap Sensor
 * Author: Yasin Lee <yasin.lee.x@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/byteorder/generic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <asm/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/types.h>

#define HX9023S_CHIP_ID 0x1D
#define HX9023S_CH_NUM 5
#define HX9023S_2BYTES 2
#define HX9023S_3BYTES 3
#define HX9023S_BYTES_MAX HX9023S_3BYTES

#define HX9023S_PRF_CFG                        0x02
#define HX9023S_CH0_CFG_7_0                    0x03
#define HX9023S_CH4_CFG_9_8                    0x0C
#define HX9023S_RANGE_7_0                      0x0D
#define HX9023S_RANGE_9_8                      0x0E
#define HX9023S_RANGE_18_16                    0x0F
#define HX9023S_AVG0_NOSR0_CFG                 0x10
#define HX9023S_NOSR12_CFG                     0x11
#define HX9023S_NOSR34_CFG                     0x12
#define HX9023S_AVG12_CFG                      0x13
#define HX9023S_AVG34_CFG                      0x14
#define HX9023S_OFFSET_DAC0_7_0                0x15
#define HX9023S_OFFSET_DAC4_9_8                0x1E
#define HX9023S_SAMPLE_NUM_7_0                 0x1F
#define HX9023S_INTEGRATION_NUM_7_0            0x21
#define HX9023S_CH_NUM_CFG                     0x24
#define HX9023S_LP_ALP_4_CFG                   0x29
#define HX9023S_LP_ALP_1_0_CFG                 0x2A
#define HX9023S_LP_ALP_3_2_CFG                 0x2B
#define HX9023S_UP_ALP_1_0_CFG                 0x2C
#define HX9023S_UP_ALP_3_2_CFG                 0x2D
#define HX9023S_DN_UP_ALP_0_4_CFG              0x2E
#define HX9023S_DN_ALP_2_1_CFG                 0x2F
#define HX9023S_DN_ALP_4_3_CFG                 0x30
#define HX9023S_RAW_BL_RD_CFG                  0x38
#define HX9023S_INTERRUPT_CFG                  0x39
#define HX9023S_INTERRUPT_CFG1                 0x3A
#define HX9023S_CALI_DIFF_CFG                  0x3B
#define HX9023S_DITHER_CFG                     0x3C
#define HX9023S_DEVICE_ID                      0x60
#define HX9023S_PROX_STATUS                    0x6B
#define HX9023S_PROX_INT_HIGH_CFG              0x6C
#define HX9023S_PROX_INT_LOW_CFG               0x6D
#define HX9023S_PROX_HIGH_DIFF_CFG_CH0_0       0x80
#define HX9023S_PROX_LOW_DIFF_CFG_CH0_0        0x88
#define HX9023S_PROX_LOW_DIFF_CFG_CH3_1        0x8F
#define HX9023S_PROX_HIGH_DIFF_CFG_CH4_0       0x9E
#define HX9023S_PROX_HIGH_DIFF_CFG_CH4_1       0x9F
#define HX9023S_PROX_LOW_DIFF_CFG_CH4_0        0xA2
#define HX9023S_PROX_LOW_DIFF_CFG_CH4_1        0xA3
#define HX9023S_CAP_INI_CH4_0                  0xB3
#define HX9023S_LP_DIFF_CH4_2                  0xBA
#define HX9023S_RAW_BL_CH4_0                   0xB5
#define HX9023S_LP_DIFF_CH4_0                  0xB8
#define HX9023S_DSP_CONFIG_CTRL1               0xC8
#define HX9023S_CAP_INI_CH0_0                  0xE0
#define HX9023S_RAW_BL_CH0_0                   0xE8
#define HX9023S_LP_DIFF_CH0_0                  0xF4
#define HX9023S_LP_DIFF_CH3_2                  0xFF

#define HX9023S_DATA_LOCK_MASK BIT(4)
#define HX9023S_INTERRUPT_MASK GENMASK(9, 0)
#define HX9023S_PROX_DEBOUNCE_MASK GENMASK(3, 0)

struct hx9023s_addr_val_pair {
	u8 addr;
	u8 val;
};

struct hx9023s_ch_data {
	int raw;
	int lp; /* low pass */
	int bl; /* base line */
	int diff; /* lp-bl */

	struct {
		int near;
		int far;
	} thres;

	u16 dac;
	u8 cs_position;
	u8 channel_positive;
	u8 channel_negative;
	bool sel_bl;
	bool sel_raw;
	bool sel_diff;
	bool sel_lp;
	bool enable;
};

struct hx9023s_data {
	struct iio_trigger *trig;
	struct regmap *regmap;
	unsigned long chan_prox_stat;
	unsigned long chan_read;
	unsigned long chan_event;
	unsigned long ch_en_stat;
	unsigned long chan_in_use;
	unsigned int prox_state_reg;
	bool trigger_enabled;

	struct {
		__be16 channels[HX9023S_CH_NUM];
		s64 ts __aligned(8);
	} buffer;

	struct hx9023s_ch_data ch_data[HX9023S_CH_NUM];
	struct mutex mutex;
};

static struct hx9023s_addr_val_pair hx9023s_reg_init_list[] = {
	/* scan period */
	{ HX9023S_PRF_CFG,                    0x17 },

	/* full scale of conversion phase of each channel */
	{ HX9023S_RANGE_7_0,                  0x11 },
	{ HX9023S_RANGE_9_8,                  0x02 },
	{ HX9023S_RANGE_18_16,                0x00 },

	/* adc avg number and osr number of each channel */
	{ HX9023S_AVG0_NOSR0_CFG,             0x71 },
	{ HX9023S_NOSR12_CFG,                 0x44 },
	{ HX9023S_NOSR34_CFG,                 0x00 },
	{ HX9023S_AVG12_CFG,                  0x33 },
	{ HX9023S_AVG34_CFG,                  0x00 },

	/* sample & integration frequency of the adc */
	{ HX9023S_SAMPLE_NUM_7_0,             0x65 },
	{ HX9023S_INTEGRATION_NUM_7_0,        0x65 },

	/* coefficient of the first order low pass filter during each channel */
	{ HX9023S_LP_ALP_1_0_CFG,             0x22 },
	{ HX9023S_LP_ALP_3_2_CFG,             0x22 },
	{ HX9023S_LP_ALP_4_CFG,               0x02 },

	/* up coefficient of the first order low pass filter during each channel */
	{ HX9023S_UP_ALP_1_0_CFG,             0x88 },
	{ HX9023S_UP_ALP_3_2_CFG,             0x88 },
	{ HX9023S_DN_UP_ALP_0_4_CFG,          0x18 },

	/* down coefficient of the first order low pass filter during each channel */
	{ HX9023S_DN_ALP_2_1_CFG,             0x11 },
	{ HX9023S_DN_ALP_4_3_CFG,             0x11 },

	/* output data */
	{ HX9023S_RAW_BL_RD_CFG,              0xF0 },

	/* enable the interrupt function */
	{ HX9023S_INTERRUPT_CFG,              0xFF },
	{ HX9023S_INTERRUPT_CFG1,             0x3B },
	{ HX9023S_DITHER_CFG,                 0x21 },

	/* threshold of the offset compensation */
	{ HX9023S_CALI_DIFF_CFG,              0x07 },

	/* proximity persistency number(near & far) */
	{ HX9023S_PROX_INT_HIGH_CFG,          0x01 },
	{ HX9023S_PROX_INT_LOW_CFG,           0x01 },

	/* disable the data lock */
	{ HX9023S_DSP_CONFIG_CTRL1,           0x00 },
};

static const struct iio_event_spec hx9023s_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_shared_by_all = BIT(IIO_EV_INFO_PERIOD),
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_shared_by_all = BIT(IIO_EV_INFO_PERIOD),
		.mask_separate = BIT(IIO_EV_INFO_VALUE),

	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

#define HX9023S_CHANNEL(idx)					\
{								\
	.type = IIO_PROXIMITY,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),\
	.indexed = 1,						\
	.channel = idx,						\
	.address = 0,						\
	.event_spec = hx9023s_events,				\
	.num_event_specs = ARRAY_SIZE(hx9023s_events),		\
	.scan_index = idx,					\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 16,					\
		.storagebits = 16,				\
		.endianness = IIO_BE,				\
	},							\
}

static const struct iio_chan_spec hx9023s_channels[] = {
	HX9023S_CHANNEL(0),
	HX9023S_CHANNEL(1),
	HX9023S_CHANNEL(2),
	HX9023S_CHANNEL(3),
	HX9023S_CHANNEL(4),
	IIO_CHAN_SOFT_TIMESTAMP(5),
};

static const unsigned int hx9023s_samp_freq_table[] = {
	2, 2, 4, 6, 8, 10, 14, 18, 22, 26,
	30, 34, 38, 42, 46, 50, 56, 62, 68, 74,
	80, 90, 100, 200, 300, 400, 600, 800, 1000, 2000,
	3000, 4000,
};

static const struct regmap_range hx9023s_wr_reg_ranges[] = {
	regmap_reg_range(HX9023S_DSP_CONFIG_CTRL1, HX9023S_DSP_CONFIG_CTRL1),
	regmap_reg_range(HX9023S_CH0_CFG_7_0, HX9023S_CH4_CFG_9_8),
	regmap_reg_range(HX9023S_PROX_HIGH_DIFF_CFG_CH0_0, HX9023S_PROX_LOW_DIFF_CFG_CH3_1),
	regmap_reg_range(HX9023S_PROX_HIGH_DIFF_CFG_CH4_0, HX9023S_PROX_HIGH_DIFF_CFG_CH4_1),
	regmap_reg_range(HX9023S_PROX_LOW_DIFF_CFG_CH4_0, HX9023S_PROX_LOW_DIFF_CFG_CH4_1),
	regmap_reg_range(HX9023S_OFFSET_DAC0_7_0, HX9023S_OFFSET_DAC4_9_8),
};

static const struct regmap_range hx9023s_volatile_reg_ranges[] = {
	regmap_reg_range(HX9023S_CAP_INI_CH4_0, HX9023S_LP_DIFF_CH4_2),
	regmap_reg_range(HX9023S_CAP_INI_CH0_0, HX9023S_LP_DIFF_CH3_2),
	regmap_reg_range(HX9023S_PROX_STATUS, HX9023S_PROX_STATUS),
};

static const struct regmap_access_table hx9023s_wr_regs = {
	.yes_ranges = hx9023s_wr_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(hx9023s_wr_reg_ranges),
};

static const struct regmap_access_table hx9023s_volatile_regs = {
	.yes_ranges = hx9023s_volatile_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(hx9023s_volatile_reg_ranges),
};

static const struct regmap_config hx9023s_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.wr_table = &hx9023s_wr_regs,
	.volatile_table = &hx9023s_volatile_regs,
};

static int hx9023s_interrupt_enable(struct hx9023s_data *data)
{
	return regmap_update_bits(data->regmap, HX9023S_INTERRUPT_CFG,
				HX9023S_INTERRUPT_MASK, HX9023S_INTERRUPT_MASK);
}

static int hx9023s_interrupt_disable(struct hx9023s_data *data)
{
	return regmap_update_bits(data->regmap, HX9023S_INTERRUPT_CFG,
				HX9023S_INTERRUPT_MASK, 0x00);
}

static int hx9023s_data_lock(struct hx9023s_data *data, bool locked)
{
	int ret;

	if (locked)
		ret = regmap_update_bits(data->regmap, HX9023S_DSP_CONFIG_CTRL1,
					HX9023S_DATA_LOCK_MASK, HX9023S_DATA_LOCK_MASK);
	else
		ret = regmap_update_bits(data->regmap, HX9023S_DSP_CONFIG_CTRL1, GENMASK(4, 3), 0);

	return ret;
}

static int hx9023s_ch_cfg(struct hx9023s_data *data)
{
	int ret;
	int i;
	u16 reg;
	u8 reg_list[HX9023S_CH_NUM * 2];
	u8 ch_pos[HX9023S_CH_NUM];
	u8 ch_neg[HX9023S_CH_NUM];

	data->ch_data[0].cs_position = 0;
	data->ch_data[1].cs_position = 2;
	data->ch_data[2].cs_position = 4;
	data->ch_data[3].cs_position = 6;
	data->ch_data[4].cs_position = 8;

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		if (data->ch_data[i].channel_positive == 255)
			ch_pos[i] = 16;
		else
			ch_pos[i] = data->ch_data[data->ch_data[i].channel_positive].cs_position;
		if (data->ch_data[i].channel_negative == 255)
			ch_neg[i] = 16;
		else
			ch_neg[i] = data->ch_data[data->ch_data[i].channel_negative].cs_position;
	}

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		reg = (u16)((0x03 << ch_pos[i]) | (0x02 << ch_neg[i]));
		reg_list[i * 2] = (u8)(reg);
		reg_list[i * 2 + 1] = (u8)(reg >> 8);
	}

	ret = regmap_bulk_write(data->regmap, HX9023S_CH0_CFG_7_0, reg_list, HX9023S_CH_NUM * 2);

	return ret;
}

static int hx9023s_reg_init(struct hx9023s_data *data)
{
	int i = 0;
	int ret;

	for (i = 0; i < (int)ARRAY_SIZE(hx9023s_reg_init_list); i++) {
		ret = regmap_bulk_write(data->regmap, hx9023s_reg_init_list[i].addr,
					&hx9023s_reg_init_list[i].val, 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int hx9023s_write_far_debounce(struct hx9023s_data *data, int val)
{
	int ret;

	if (val > 0xF)
		val = 0xF;

	guard(mutex)(&data->mutex);
	ret = regmap_update_bits(data->regmap, HX9023S_PROX_INT_LOW_CFG,
				HX9023S_PROX_DEBOUNCE_MASK, val);

	return ret;
}

static int hx9023s_write_near_debounce(struct hx9023s_data *data, int val)
{
	int ret;

	if (val > 0xF)
		val = 0xF;

	guard(mutex)(&data->mutex);
	ret = regmap_update_bits(data->regmap, HX9023S_PROX_INT_HIGH_CFG,
				HX9023S_PROX_DEBOUNCE_MASK, val);

	return ret;
}

static int hx9023s_read_far_debounce(struct hx9023s_data *data, int *val)
{
	int ret;

	ret = regmap_read(data->regmap, HX9023S_PROX_INT_LOW_CFG, val);
	if (ret)
		return ret;

	*val = FIELD_GET(HX9023S_PROX_DEBOUNCE_MASK, *val);

	return IIO_VAL_INT;
}

static int hx9023s_read_near_debounce(struct hx9023s_data *data, int *val)
{
	int ret;

	ret = regmap_read(data->regmap, HX9023S_PROX_INT_HIGH_CFG, val);
	if (ret)
		return ret;

	*val = FIELD_GET(HX9023S_PROX_DEBOUNCE_MASK, *val);

	return IIO_VAL_INT;
}

static int hx9023s_get_thres_near(struct hx9023s_data *data, u8 ch, int *val)
{
	int ret;
	u8 buf[2];

	if (ch == 4)
		ret = regmap_bulk_read(data->regmap, HX9023S_PROX_HIGH_DIFF_CFG_CH4_0, buf, 2);
	else
		ret = regmap_bulk_read(data->regmap,
				HX9023S_PROX_HIGH_DIFF_CFG_CH0_0 + (ch * HX9023S_2BYTES), buf, 2);

	if (ret)
		return ret;

	*val = get_unaligned_le16(buf);
	*val = (*val & GENMASK(9, 0)) * 32;
	data->ch_data[ch].thres.near = *val;

	return IIO_VAL_INT;
}

static int hx9023s_get_thres_far(struct hx9023s_data *data, u8 ch, int *val)
{
	int ret;
	u8 buf[2];

	if (ch == 4)
		ret = regmap_bulk_read(data->regmap, HX9023S_PROX_LOW_DIFF_CFG_CH4_0, buf, 2);
	else
		ret = regmap_bulk_read(data->regmap,
				HX9023S_PROX_LOW_DIFF_CFG_CH0_0 + (ch * HX9023S_2BYTES), buf, 2);

	if (ret)
		return ret;

	*val = get_unaligned_le16(buf);
	*val = (*val & GENMASK(9, 0)) * 32;
	data->ch_data[ch].thres.far = *val;

	return IIO_VAL_INT;
}

static int hx9023s_set_thres_near(struct hx9023s_data *data, u8 ch, int val)
{
	int ret;
	__le16 val_le16 = cpu_to_le16((val / 32) & GENMASK(9, 0));

	data->ch_data[ch].thres.near = ((val / 32) & GENMASK(9, 0)) * 32;

	if (ch == 4)
		ret = regmap_bulk_write(data->regmap, HX9023S_PROX_HIGH_DIFF_CFG_CH4_0,
					&val_le16, sizeof(val_le16));
	else
		ret = regmap_bulk_write(data->regmap,
					HX9023S_PROX_HIGH_DIFF_CFG_CH0_0 + (ch * HX9023S_2BYTES),
					&val_le16, sizeof(val_le16));

	return ret;
}

static int hx9023s_set_thres_far(struct hx9023s_data *data, u8 ch, int val)
{
	int ret;
	__le16 val_le16 = cpu_to_le16((val / 32) & GENMASK(9, 0));

	data->ch_data[ch].thres.far = ((val / 32) & GENMASK(9, 0)) * 32;

	if (ch == 4)
		ret = regmap_bulk_write(data->regmap, HX9023S_PROX_LOW_DIFF_CFG_CH4_0,
					&val_le16, sizeof(val_le16));
	else
		ret = regmap_bulk_write(data->regmap,
					HX9023S_PROX_LOW_DIFF_CFG_CH0_0 + (ch * HX9023S_2BYTES),
					&val_le16, sizeof(val_le16));

	return ret;
}

static void hx9023s_get_prox_state(struct hx9023s_data *data)
{
	int ret;

	ret = regmap_read(data->regmap, HX9023S_PROX_STATUS, &data->prox_state_reg);
	if (ret)
		return;
}

static void hx9023s_data_select(struct hx9023s_data *data)
{
	int ret;
	int i;
	unsigned long buf[1];

	ret = regmap_read(data->regmap, HX9023S_RAW_BL_RD_CFG, (unsigned int *)buf);
	if (ret)
		return;

	for (i = 0; i < 4; i++) {
		data->ch_data[i].sel_diff = test_bit(i, buf);
		data->ch_data[i].sel_lp = !data->ch_data[i].sel_diff;
		data->ch_data[i].sel_bl = test_bit(i + 4, buf);
		data->ch_data[i].sel_raw = !data->ch_data[i].sel_bl;
	}

	ret = regmap_read(data->regmap, HX9023S_INTERRUPT_CFG1, (unsigned int *)buf);
	if (ret)
		return;

	data->ch_data[4].sel_diff = test_bit(2, buf);
	data->ch_data[4].sel_lp = !data->ch_data[4].sel_diff;
	data->ch_data[4].sel_bl = test_bit(3, buf);
	data->ch_data[4].sel_raw = !data->ch_data[4].sel_bl;
}

static int hx9023s_sample(struct hx9023s_data *data)
{
	int ret;
	int i;
	u8 data_size;
	u8 offset_data_size;
	int value;
	u8 rx_buf[HX9023S_CH_NUM * HX9023S_BYTES_MAX];

	hx9023s_data_lock(data, true);
	hx9023s_data_select(data);

	data_size = HX9023S_3BYTES;

	/* ch0~ch3 */
	ret = regmap_bulk_read(data->regmap, HX9023S_RAW_BL_CH0_0, rx_buf,
				(HX9023S_CH_NUM * data_size) - data_size);
	if (ret)
		return ret;

	/* ch4 */
	ret = regmap_bulk_read(data->regmap, HX9023S_RAW_BL_CH4_0,
				rx_buf + ((HX9023S_CH_NUM * data_size) - data_size), data_size);
	if (ret)
		return ret;

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		value = get_unaligned_le16(&rx_buf[i * data_size + 1]);
		value = sign_extend32(value, 15);
		data->ch_data[i].raw = 0;
		data->ch_data[i].bl = 0;
		if (data->ch_data[i].sel_raw == true)
			data->ch_data[i].raw = value;
		if (data->ch_data[i].sel_bl == true)
			data->ch_data[i].bl = value;
	}

	/* ch0~ch3 */
	ret = regmap_bulk_read(data->regmap, HX9023S_LP_DIFF_CH0_0, rx_buf,
				(HX9023S_CH_NUM * data_size) - data_size);
	if (ret)
		return ret;

	/* ch4 */
	ret = regmap_bulk_read(data->regmap, HX9023S_LP_DIFF_CH4_0,
				rx_buf + ((HX9023S_CH_NUM * data_size) - data_size), data_size);
	if (ret)
		return ret;

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		value = get_unaligned_le16(&rx_buf[i * data_size + 1]);
		value = sign_extend32(value, 15);
		data->ch_data[i].lp = 0;
		data->ch_data[i].diff = 0;
		if (data->ch_data[i].sel_lp == true)
			data->ch_data[i].lp = value;
		if (data->ch_data[i].sel_diff == true)
			data->ch_data[i].diff = value;
	}

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		if (data->ch_data[i].sel_lp == true && data->ch_data[i].sel_bl == true)
			data->ch_data[i].diff = data->ch_data[i].lp - data->ch_data[i].bl;
	}

	/* offset dac */
	offset_data_size = HX9023S_2BYTES;
	ret = regmap_bulk_read(data->regmap, HX9023S_OFFSET_DAC0_7_0, rx_buf,
				(HX9023S_CH_NUM * offset_data_size));
	if (ret)
		return ret;

	for (i = 0; i < HX9023S_CH_NUM; i++) {
		value = get_unaligned_le16(&rx_buf[i * offset_data_size]);
		value = FIELD_GET(GENMASK(11, 0), value);
		data->ch_data[i].dac = value;
	}

	hx9023s_data_lock(data, false);

	return 0;
}

static int hx9023s_ch_en(struct hx9023s_data *data, u8 ch_id, bool en)
{
	int ret;
	unsigned int buf;

	ret = regmap_read(data->regmap, HX9023S_CH_NUM_CFG, &buf);
	if (ret)
		return ret;

	data->ch_en_stat = buf;

	if (en) {
		if (data->ch_en_stat == 0)
			data->prox_state_reg = 0;
		set_bit(ch_id, &data->ch_en_stat);
		ret = regmap_bulk_write(data->regmap, HX9023S_CH_NUM_CFG, &data->ch_en_stat, 1);
		if (ret)
			return ret;
		data->ch_data[ch_id].enable = true;
	} else {
		clear_bit(ch_id, &data->ch_en_stat);
		ret = regmap_bulk_write(data->regmap, HX9023S_CH_NUM_CFG, &data->ch_en_stat, 1);
		if (ret)
			return ret;
		data->ch_data[ch_id].enable = false;
	}

	return 0;
}

static int hx9023s_property_get(struct hx9023s_data *data)
{
	int ret, i;
	u32 temp;
	struct fwnode_handle *child;
	struct device *dev = regmap_get_device(data->regmap);

	ret = device_property_read_u32(dev, "channel-in-use", &temp);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read channel-in-use property\n");
	data->chan_in_use = temp;

	device_for_each_child_node(dev, child) {
		ret = fwnode_property_read_u32(child, "channel-positive", &temp);
		if (ret)
			return dev_err_probe(dev, ret,
					"Failed to read channel-positive for channel %d\n", i);
		data->ch_data[i].channel_positive = temp;

		ret = fwnode_property_read_u32(child, "channel-negative", &temp);
		if (ret)
			return dev_err_probe(dev, ret,
					"Failed to read channel-negative for channel %d\n", i);
		data->ch_data[i].channel_negative = temp;

		i++;
	}

	return 0;
}

static int hx9023s_update_chan_en(struct hx9023s_data *data,
				unsigned long chan_read,
				unsigned long chan_event)
{
	int i;
	unsigned long channels = chan_read | chan_event;

	if ((data->chan_read | data->chan_event) != channels) {
		for_each_set_bit(i, &channels, HX9023S_CH_NUM)
			hx9023s_ch_en(data, i, test_bit(i, &data->chan_in_use));
		for_each_clear_bit(i, &channels, HX9023S_CH_NUM)
			hx9023s_ch_en(data, i, false);
	}

	data->chan_read = chan_read;
	data->chan_event = chan_event;

	return 0;
}

static int hx9023s_get_proximity(struct hx9023s_data *data,
				const struct iio_chan_spec *chan,
				int *val)
{
	hx9023s_sample(data);
	hx9023s_get_prox_state(data);
	*val = data->ch_data[chan->channel].diff;
	return IIO_VAL_INT;
}

static int hx9023s_get_samp_freq(struct hx9023s_data *data, int *val, int *val2)
{
	int ret;
	unsigned int odr;
	unsigned int buf;

	ret = regmap_read(data->regmap, HX9023S_PRF_CFG, &buf);
	if (ret)
		return ret;

	odr = hx9023s_samp_freq_table[buf];
	*val = 1000 / odr;
	*val2 = div_u64((1000 % odr) * 1000000ULL, odr);

	return IIO_VAL_INT_PLUS_MICRO;
}

static int hx9023s_read_raw(struct iio_dev *indio_dev, const struct iio_chan_spec *chan,
				int *val, int *val2, long mask)
{
	struct hx9023s_data *data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		ret = hx9023s_get_proximity(data, chan, val);
		iio_device_release_direct_mode(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return hx9023s_get_samp_freq(data, val, val2);
	default:
		return -EINVAL;
	}
}

static int hx9023s_set_samp_freq(struct hx9023s_data *data, int val, int val2)
{
	int i;
	int ret;
	int period_ms;
	struct device *dev = regmap_get_device(data->regmap);

	period_ms = div_u64(1000000000ULL, (val * 1000000ULL + val2));

	for (i = 0; i < ARRAY_SIZE(hx9023s_samp_freq_table); i++) {
		if (period_ms == hx9023s_samp_freq_table[i])
			break;
	}
	if (i == ARRAY_SIZE(hx9023s_samp_freq_table)) {
		dev_err(dev, "Period:%dms NOT found!\n", period_ms);
		return -EINVAL;
	}

	ret = regmap_bulk_write(data->regmap, HX9023S_PRF_CFG, &i, 1);

	return ret;
}

static int hx9023s_write_raw(struct iio_dev *indio_dev, const struct iio_chan_spec *chan,
				int val, int val2, long mask)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	return hx9023s_set_samp_freq(data, val, val2);
}

static irqreturn_t hx9023s_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct hx9023s_data *data = iio_priv(indio_dev);

	if (data->trigger_enabled)
		iio_trigger_poll(data->trig);

	return IRQ_WAKE_THREAD;
}

static void hx9023s_push_events(struct iio_dev *indio_dev)
{
	struct hx9023s_data *data = iio_priv(indio_dev);
	s64 timestamp = iio_get_time_ns(indio_dev);
	unsigned long prox_changed;
	unsigned int chan;

	hx9023s_sample(data);
	hx9023s_get_prox_state(data);

	prox_changed = (data->chan_prox_stat ^ data->prox_state_reg) & data->chan_event;

	for_each_set_bit(chan, &prox_changed, HX9023S_CH_NUM) {
		int dir;

		dir = (data->prox_state_reg & BIT(chan)) ? IIO_EV_DIR_FALLING : IIO_EV_DIR_RISING;

		iio_push_event(indio_dev,
			IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, chan, IIO_EV_TYPE_THRESH, dir),
			timestamp);
	}
	data->chan_prox_stat = data->prox_state_reg;
}

static irqreturn_t hx9023s_irq_thread_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct hx9023s_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->mutex);
	hx9023s_push_events(indio_dev);

	return IRQ_HANDLED;
}

static int hx9023s_read_event_val(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 enum iio_event_type type,
				 enum iio_event_direction dir,
				 enum iio_event_info info, int *val, int *val2)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return hx9023s_get_thres_far(data, chan->channel, val);
		case IIO_EV_DIR_FALLING:
			return hx9023s_get_thres_near(data, chan->channel, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_INFO_PERIOD:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return hx9023s_read_far_debounce(data, val);
		case IIO_EV_DIR_FALLING:
			return hx9023s_read_near_debounce(data, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int hx9023s_write_event_val(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  enum iio_event_type type,
				  enum iio_event_direction dir,
				  enum iio_event_info info, int val, int val2)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return hx9023s_set_thres_far(data, chan->channel, val);
		case IIO_EV_DIR_FALLING:
			return hx9023s_set_thres_near(data, chan->channel, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_INFO_PERIOD:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return hx9023s_write_far_debounce(data, val);
		case IIO_EV_DIR_FALLING:
			return hx9023s_write_near_debounce(data, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int hx9023s_read_event_config(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	return test_bit(chan->channel, &data->chan_event);
}

static int hx9023s_write_event_config(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				int state)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	if (test_bit(chan->channel, &data->chan_in_use)) {
		hx9023s_ch_en(data, chan->channel, !!state);
		if (data->ch_data[chan->channel].enable)
			set_bit(chan->channel, &data->chan_event);
		else
			clear_bit(chan->channel, &data->chan_event);
	}

	return 0;
}

static const struct iio_info hx9023s_info = {
	.read_raw = hx9023s_read_raw,
	.write_raw = hx9023s_write_raw,
	.read_event_value = hx9023s_read_event_val,
	.write_event_value = hx9023s_write_event_val,
	.read_event_config = hx9023s_read_event_config,
	.write_event_config = hx9023s_write_event_config,
};

static int hx9023s_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct hx9023s_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->mutex);
	if (state)
		hx9023s_interrupt_enable(data);
	else if (!data->chan_read)
		hx9023s_interrupt_disable(data);
	data->trigger_enabled = state;

	return 0;
}

static const struct iio_trigger_ops hx9023s_trigger_ops = {
	.set_trigger_state = hx9023s_set_trigger_state,
};

static irqreturn_t hx9023s_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct hx9023s_data *data = iio_priv(indio_dev);
	int bit;
	int i;

	guard(mutex)(&data->mutex);
	hx9023s_sample(data);
	hx9023s_get_prox_state(data);

	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength)
		data->buffer.channels[i++] =
			cpu_to_be16(data->ch_data[indio_dev->channels[bit].channel].diff);

	iio_push_to_buffers_with_timestamp(indio_dev, &data->buffer, pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int hx9023s_buffer_preenable(struct iio_dev *indio_dev)
{
	struct hx9023s_data *data = iio_priv(indio_dev);
	unsigned long channels;
	int bit;

	guard(mutex)(&data->mutex);
	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength)
		__set_bit(indio_dev->channels[bit].channel, &channels);

	hx9023s_update_chan_en(data, channels, data->chan_event);

	return 0;
}

static int hx9023s_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct hx9023s_data *data = iio_priv(indio_dev);

	guard(mutex)(&data->mutex);
	hx9023s_update_chan_en(data, 0, data->chan_event);

	return 0;
}

static const struct iio_buffer_setup_ops hx9023s_buffer_setup_ops = {
	.preenable = hx9023s_buffer_preenable,
	.postdisable = hx9023s_buffer_postdisable,
};

static int hx9023s_probe(struct i2c_client *client)
{
	int ret;
	unsigned int id;
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct hx9023s_data *data;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct hx9023s_data));
	if (!indio_dev)
		return dev_err_probe(dev, -ENOMEM, "device alloc failed\n");

	data = iio_priv(indio_dev);
	mutex_init(&data->mutex);

	data->regmap = devm_regmap_init_i2c(client, &hx9023s_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap), "regmap init failed\n");

	ret = hx9023s_property_get(data);
	if (ret)
		return dev_err_probe(dev, ret, "dts phase failed\n");

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "regulator get failed\n");

	fsleep(1000);

	ret = regmap_read(data->regmap, HX9023S_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "id check failed\n");

	indio_dev->channels = hx9023s_channels;
	indio_dev->num_channels = ARRAY_SIZE(hx9023s_channels);
	indio_dev->info = &hx9023s_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = "hx9023s";
	i2c_set_clientdata(client, indio_dev);

	ret = hx9023s_reg_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "device init failed\n");

	ret = hx9023s_ch_cfg(data);
	if (ret)
		return dev_err_probe(dev, ret, "channel config failed\n");

	ret = regcache_sync(data->regmap);
	if (ret)
		return dev_err_probe(dev, ret, "regcache sync failed\n");

	if (client->irq) {
		ret = devm_request_threaded_irq(dev, client->irq, hx9023s_irq_handler,
						hx9023s_irq_thread_handler, IRQF_ONESHOT,
						"hx9023s_event", indio_dev);
		if (ret)
			return dev_err_probe(dev, ret, "irq request failed\n");

		data->trig = devm_iio_trigger_alloc(dev, "%s-dev%d", indio_dev->name,
						iio_device_id(indio_dev));
		if (!data->trig)
			return dev_err_probe(dev, -ENOMEM,
					"iio trigger alloc failed\n");

		data->trig->ops = &hx9023s_trigger_ops;
		iio_trigger_set_drvdata(data->trig, indio_dev);

		ret = devm_iio_trigger_register(dev, data->trig);
		if (ret)
			return dev_err_probe(dev, ret,
					"iio trigger register failed\n");
	}

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, iio_pollfunc_store_time,
					hx9023s_trigger_handler, &hx9023s_buffer_setup_ops);
	if (ret)
		return dev_err_probe(dev, ret,
				"iio triggered buffer setup failed\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "iio device register failed\n");

	return 0;
}

static int hx9023s_suspend(struct device *dev)
{
	struct hx9023s_data *data = iio_priv(dev_get_drvdata(dev));

	hx9023s_interrupt_disable(data);

	return 0;
}

static int hx9023s_resume(struct device *dev)
{
	struct hx9023s_data *data = iio_priv(dev_get_drvdata(dev));

	hx9023s_interrupt_enable(data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(hx9023s_pm_ops, hx9023s_suspend, hx9023s_resume);

static const struct acpi_device_id hx9023s_acpi_match[] = {
	{ "TYHX9023" },
	{}
};
MODULE_DEVICE_TABLE(acpi, hx9023s_acpi_match);

static const struct of_device_id hx9023s_of_match[] = {
	{ .compatible = "tyhx,hx9023s" },
	{}
};
MODULE_DEVICE_TABLE(of, hx9023s_of_match);

static const struct i2c_device_id hx9023s_id[] = {
	{ "hx9023s" },
	{}
};
MODULE_DEVICE_TABLE(i2c, hx9023s_id);

static struct i2c_driver hx9023s_driver = {
	.driver = {
		.name = "hx9023s",
		.acpi_match_table = hx9023s_acpi_match,
		.of_match_table = hx9023s_of_match,
		.pm = &hx9023s_pm_ops,

		/*
		 * The I2C operations in hx9023s_reg_init() and hx9023s_ch_cfg()
		 * are time-consuming. prefer async so we don't delay boot
		 * if we're builtin to the kernel.
		 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = hx9023s_probe,
	.id_table = hx9023s_id,
};
module_i2c_driver(hx9023s_driver);

MODULE_AUTHOR("Yasin Lee <yasin.lee.x@gmail.com>");
MODULE_DESCRIPTION("Driver for TYHX HX9023S SAR sensor");
MODULE_LICENSE("GPL");
