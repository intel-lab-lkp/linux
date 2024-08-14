// SPDX-License-Identifier: GPL-2.0
/*
 * AWINIC aw96103 proximity sensor driver
 *
 * Author: Wang Shuaijie <wangshuaijie@awinic.com>
 *
 * Copyright (c) 2024 awinic Technology CO., LTD
 */
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include "aw96103.h"

static const unsigned int aw96103_reg_default[] = {
	0x0000, 0x00003f3f, 0x0004, 0x00000064, 0x0008, 0x0017c11e,
	0x000c, 0x05000000, 0x0010, 0x00093ffd, 0x0014, 0x19240009,
	0x0018, 0xd81c0207, 0x001c, 0xff000000, 0x0020, 0x00241900,
	0x0024, 0x00093ff7, 0x0028, 0x58020009, 0x002c, 0xd81c0207,
	0x0030, 0xff000000, 0x0034, 0x00025800, 0x0038, 0x00093fdf,
	0x003c, 0x7d3b0009, 0x0040, 0xd81c0207,	0x0044, 0xff000000,
	0x0048, 0x003b7d00, 0x004c, 0x00093f7f, 0x0050, 0xe9310009,
	0x0054, 0xd81c0207, 0x0058, 0xff000000,	0x005c, 0x0031e900,
	0x0060, 0x00093dff, 0x0064, 0x1a0c0009,	0x0068, 0xd81c0207,
	0x006c, 0xff000000, 0x0070, 0x000c1a00,	0x0074, 0x80093fff,
	0x0078, 0x043d0009, 0x007c, 0xd81c0207,	0x0080, 0xff000000,
	0x0084, 0x003d0400, 0x00a0, 0xe6400000,	0x00a4, 0x00000000,
	0x00a8, 0x010408d2, 0x00ac, 0x00000000,	0x00b0, 0x00000000,
	0x00b8, 0x00005fff, 0x00bc, 0x00000000,	0x00c0, 0x00000000,
	0x00c4, 0x00000000, 0x00c8, 0x00000000,	0x00cc, 0x00000000,
	0x00d0, 0x00000000, 0x00d4, 0x00000000, 0x00d8, 0x00000000,
	0x00dc, 0xe6447800, 0x00e0, 0x78000000,	0x00e4, 0x010408d2,
	0x00e8, 0x00000000, 0x00ec, 0x00000000,	0x00f4, 0x00005fff,
	0x00f8, 0x00000000, 0x00fc, 0x00000000,	0x0100, 0x00000000,
	0x0104, 0x00000000, 0x0108, 0x00000000,	0x010c, 0x02000000,
	0x0110, 0x00000000, 0x0114, 0x00000000,	0x0118, 0xe6447800,
	0x011c, 0x78000000, 0x0120, 0x010408d2,	0x0124, 0x00000000,
	0x0128, 0x00000000, 0x0130, 0x00005fff,	0x0134, 0x00000000,
	0x0138, 0x00000000, 0x013c, 0x00000000,	0x0140, 0x00000000,
	0x0144, 0x00000000, 0x0148, 0x02000000,	0x014c, 0x00000000,
	0x0150, 0x00000000, 0x0154, 0xe6447800,	0x0158, 0x78000000,
	0x015c, 0x010408d2, 0x0160, 0x00000000,	0x0164, 0x00000000,
	0x016c, 0x00005fff, 0x0170, 0x00000000,	0x0174, 0x00000000,
	0x0178, 0x00000000, 0x017c, 0x00000000,	0x0180, 0x00000000,
	0x0184, 0x02000000, 0x0188, 0x00000000,	0x018c, 0x00000000,
	0x0190, 0xe6447800, 0x0194, 0x78000000,	0x0198, 0x010408d2,
	0x019c, 0x00000000, 0x01a0, 0x00000000,	0x01a8, 0x00005fff,
	0x01ac, 0x00000000, 0x01b0, 0x00000000,	0x01b4, 0x00000000,
	0x01b8, 0x00000000, 0x01bc, 0x00000000,	0x01c0, 0x02000000,
	0x01c4, 0x00000000, 0x01c8, 0x00000000,	0x01cc, 0xe6407800,
	0x01d0, 0x78000000, 0x01d4, 0x010408d2,	0x01d8, 0x00000000,
	0x01dc, 0x00000000, 0x01e4, 0x00005fff,	0x01e8, 0x00000000,
	0x01ec, 0x00000000, 0x01f0, 0x00000000,	0x01f4, 0x00000000,
	0x01f8, 0x00000000, 0x01fc, 0x02000000,	0x0200, 0x00000000,
	0x0204, 0x00000000, 0x0208, 0x00000008,	0x020c, 0x0000000d,
	0x41fc, 0x00000000, 0x4400, 0x00000000,	0x4410, 0x00000000,
	0x4420, 0x00000000, 0x4430, 0x00000000,	0x4440, 0x00000000,
	0x4450, 0x00000000, 0x4460, 0x00000000,	0x4470, 0x00000000,
	0xf080, 0x00003018, 0xf084, 0x00000fff,	0xf800, 0x00000000,
	0xf804, 0x00002e00, 0xf8d0, 0x00000001,	0xf8d4, 0x00000000,
	0xff00, 0x00000301, 0xff0c, 0x01000000,	0xffe0, 0x00000000,
	0xfff4, 0x00004011, 0x0090, 0x00000000,	0x0094, 0x00000000,
	0x0098, 0x00000000, 0x009c, 0x3f3f3f3f,
};

static void aw96103_parsing_bin_file(struct aw_bin *bin)
{
	int i;

	bin->valid_data_addr = AW96103_BIN_VALID_DATA_OFFSET;
	bin->valid_data_len =
		*(unsigned int *)(bin->data + AW96103_BIN_DATA_LEN_OFFSET) -
		AW96103_BIN_DATA_REG_NUM_SIZE;
	for (i = 0; i < AW96103_BIN_CHIP_TYPE_SIZE; i++) {
		bin->chip_type[i] =
			*(bin->data + AW96103_BIN_CHIP_TYPE_OFFSET + i);
	}
}

static const struct regmap_config aw96103_regmap_confg = {
	.reg_bits = 16,
	.val_bits = 32,
};

static const struct iio_event_spec aw_common_events[3] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_PERIOD),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_PERIOD),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE) |
			BIT(IIO_EV_INFO_HYSTERESIS) |
			BIT(IIO_EV_INFO_VALUE),
	}
};

static int aw96103_get_diff_raw(struct aw96103 *aw96103,
		unsigned int chan, int *buf)
{
	u32 data;
	int ret;

	ret = regmap_read(aw96103->regmap,
			AW96103_REG_DIFF_CH0 + chan * 4, &data);
	if (ret)
		return ret;
	*buf = (int)(data / AW_DATA_PROCESS_FACTOR);

	return 0;
}

static int aw96103_read_raw(struct iio_dev *indio_dev,
		const struct iio_chan_spec *chan,
		int *val, int *val2, long mask)
{
	struct aw96103 *aw96103;
	int ret;

	aw96103 = iio_priv(indio_dev);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = aw96103_get_diff_raw(aw96103, chan->channel, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int aw96103_write_thresh(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int val)
{
	unsigned int reg;
	int ret;

	reg = AW96103_REG_PROXTH0_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	mutex_lock(&aw96103->mutex);
	ret = regmap_write(aw96103->regmap, reg, val);
	mutex_unlock(&aw96103->mutex);

	return ret;
}

static int aw96103_read_thresh(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int *val)
{
	unsigned int reg;
	int ret;

	reg = AW96103_REG_PROXTH0_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	ret = regmap_read(aw96103->regmap, reg, val);
	if (ret)
		return ret;

	return IIO_VAL_INT;
}

static int aw96103_write_out_debounce(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	reg_val = FIELD_PREP(AW96103_OUTDEB_MASK, val);
	mutex_lock(&aw96103->mutex);
	ret = regmap_update_bits(aw96103->regmap, reg,
			AW96103_OUTDEB_MASK, reg_val);
	mutex_unlock(&aw96103->mutex);

	return ret;
}

static int aw96103_read_out_debounce(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int *val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	ret = regmap_read(aw96103->regmap, reg, &reg_val);
	if (ret)
		return ret;
	*val = FIELD_GET(AW96103_OUTDEB_MASK, reg_val);

	return IIO_VAL_INT;
}

static int aw96103_write_in_debounce(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	reg_val = FIELD_PREP(AW96103_INDEB_MASK, val);
	mutex_lock(&aw96103->mutex);
	ret = regmap_update_bits(aw96103->regmap, reg,
			AW96103_INDEB_MASK, reg_val);
	mutex_unlock(&aw96103->mutex);

	return ret;
}

static int aw96103_read_in_debounce(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int *val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	ret = regmap_read(aw96103->regmap, reg, &reg_val);
	if (ret)
		return ret;
	*val = FIELD_GET(AW96103_INDEB_MASK, reg_val);

	return IIO_VAL_INT;
}

static int aw96103_write_hysteresis(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	reg_val = FIELD_PREP(AW96103_THHYST_MASK, val);
	mutex_lock(&aw96103->mutex);
	ret = regmap_update_bits(aw96103->regmap, reg,
			AW96103_THHYST_MASK, reg_val);
	mutex_unlock(&aw96103->mutex);

	return ret;
}

static int aw96103_read_hysteresis(struct aw96103 *aw96103,
			       const struct iio_chan_spec *chan, int *val)
{
	unsigned int reg, reg_val;
	int ret;

	reg = AW96103_REG_PROXCTRL_CH0 + chan->channel * AW96103_PROXTH_CH_STEP;
	ret = regmap_read(aw96103->regmap, reg, &reg_val);
	if (ret)
		return ret;
	*val = FIELD_GET(AW96103_THHYST_MASK, reg_val);

	return IIO_VAL_INT;
}

static int aw96103_read_event_val(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 enum iio_event_type type,
				 enum iio_event_direction dir,
				 enum iio_event_info info, int *val, int *val2)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		return aw96103_read_thresh(aw96103, chan, val);
	case IIO_EV_INFO_PERIOD:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return aw96103_read_out_debounce(aw96103, chan, val);
		case IIO_EV_DIR_FALLING:
			return aw96103_read_in_debounce(aw96103, chan, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_INFO_HYSTERESIS:
		return aw96103_read_hysteresis(aw96103, chan, val);
	default:
		return -EINVAL;
	}
}

static int aw96103_write_event_val(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  enum iio_event_type type,
				  enum iio_event_direction dir,
				  enum iio_event_info info, int val, int val2)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		return aw96103_write_thresh(aw96103, chan, val);
	case IIO_EV_INFO_PERIOD:
		switch (dir) {
		case IIO_EV_DIR_RISING:
			return aw96103_write_out_debounce(aw96103, chan, val);
		case IIO_EV_DIR_FALLING:
			return aw96103_write_in_debounce(aw96103, chan, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_INFO_HYSTERESIS:
		return aw96103_write_hysteresis(aw96103, chan, val);
	default:
		return -EINVAL;
	}
}

static int aw96103_read_event_config(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);

	return aw96103->channels_arr[chan->channel].used;
}

static int aw96103_write_event_config(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 enum iio_event_type type,
				 enum iio_event_direction dir, int state)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);

	aw96103->channels_arr[chan->channel].used = !!state;

	if (!!state)
		return regmap_update_bits(aw96103->regmap,
				AW96103_REG_SCANCTRL0,
				BIT(chan->channel), BIT(chan->channel));
	else
		return regmap_update_bits(aw96103->regmap,
				AW96103_REG_SCANCTRL0,
				BIT(chan->channel), 0);
}

struct iio_info iio_info = {
	.read_raw = aw96103_read_raw,
	.read_event_value = aw96103_read_event_val,
	.write_event_value = aw96103_write_event_val,
	.read_event_config = aw96103_read_event_config,
	.write_event_config = aw96103_write_event_config,
};

static int aw96103_iio_init(struct iio_dev *indio_dev)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);
	struct iio_chan_spec *aw96103_channels;
	unsigned int i;

	/*
	 * There is one more logical channel than the actual channels,
	 * and the extra logical channel is used for temperature detection
	 * but not for status detection. The specific channel used for
	 * temperature detection is determined by the register configuration.
	 */
	aw96103->channels_arr = devm_kcalloc(aw96103->dev,
			aw96103->max_channels + 1,
			sizeof(struct aw_channels_info), GFP_KERNEL);
	if (!aw96103->channels_arr)
		return -ENOMEM;

	aw96103_channels = devm_kcalloc(aw96103->dev, aw96103->max_channels + 1,
			sizeof(*aw96103_channels), GFP_KERNEL);
	if (!aw96103_channels)
		return -ENOMEM;

	for (i = 0; i <= aw96103->max_channels; i++) {
		aw96103_channels[i].type = IIO_PROXIMITY;
		aw96103_channels[i].info_mask_separate =
			BIT(IIO_CHAN_INFO_RAW);
		aw96103_channels[i].indexed = 1;
		aw96103_channels[i].channel = i;
		aw96103_channels[i].event_spec = aw_common_events;
		aw96103_channels[i].num_event_specs =
			ARRAY_SIZE(aw_common_events);
	}

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->num_channels = aw96103->max_channels + 1;
	indio_dev->channels = aw96103_channels;
	indio_dev->info = &iio_info;
	indio_dev->name = "aw96103_sensor";
	indio_dev->dev.parent = aw96103->dev;

	return devm_iio_device_register(aw96103->dev, indio_dev);
}

static int aw96103_channel_scan_start(struct aw96103 *aw96103)
{
	int ret;

	ret = regmap_write(aw96103->regmap, AW96103_REG_CMD,
			AW96103_ACTIVE_MODE);
	if (ret)
		return ret;

	return regmap_write(aw96103->regmap, AW96103_REG_IRQEN,
			aw96103->hostirqen);
}

static int aw96103_reg_version_comp(struct aw96103 *aw96103,
		struct aw_bin *aw_bin)
{
	u32 blfilt1_data, blfilt1_tmp;
	unsigned char i;
	int ret;

	/*
	 * If the chip version is AW96103A and the loaded register
	 * configuration file is for AW96103, special handling of the
	 * AW96103_REG_BLRSTRNG_CH0 register is required.
	 */
	if ((aw96103->vers == AW96103A) && (aw_bin->chip_type[7] == '\0')) {
		for (i = 0; i <= aw96103->max_channels; i++) {
			ret = regmap_read(aw96103->regmap,
					AW96103_REG_BLFILT_CH0 +
					(AW96103_BLFILT_CH_STEP * i),
					&blfilt1_data);
			if (ret)
				return ret;
			blfilt1_tmp = FIELD_GET(AW96103_BLERRTRIG_MASK,
					blfilt1_data);
			if (blfilt1_tmp == 1) {
				ret = regmap_update_bits(aw96103->regmap,
					AW96103_REG_BLRSTRNG_CH0 +
					(AW96103_BLFILT_CH_STEP * i),
					AW96103_BLRSTRNG_MASK, 1 << i);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int aw96103_bin_valid_loaded(struct aw96103 *aw96103,
		struct aw_bin *aw_bin_data_s)
{
	unsigned int start_addr = aw_bin_data_s->valid_data_addr;
	u32 i, reg_data;
	u16 reg_addr;
	int ret;

	for (i = 0; i < aw_bin_data_s->valid_data_len;
	     i += 6, start_addr += 6) {
		reg_addr = *(u16 *)(aw_bin_data_s->data + start_addr);
		reg_data = *(u32 *)(aw_bin_data_s->data + start_addr + 2);
		if ((reg_addr == AW96103_REG_EEDA0) ||
		    (reg_addr == AW96103_REG_EEDA1))
			continue;
		if (reg_addr == AW96103_REG_IRQEN) {
			aw96103->hostirqen = reg_data;
			continue;
		}
		if (reg_addr == AW96103_REG_SCANCTRL0)
			aw96103->chan_en = FIELD_GET(AW96103_CHAN_EN_MASK,
					reg_data);
		ret = regmap_write(aw96103->regmap, reg_addr, reg_data);
		if (ret < 0)
			return ret;

	}
	ret = aw96103_reg_version_comp(aw96103, aw_bin_data_s);
	if (ret)
		return ret;

	return aw96103_channel_scan_start(aw96103);
}

static int aw96103_para_loaded(struct aw96103 *aw96103)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(aw96103_reg_default); i += 2) {
		ret = regmap_write(aw96103->regmap,
				(u16)aw96103_reg_default[i],
				(u32)aw96103_reg_default[i + 1]);
		if (ret)
			return ret;
		if (aw96103_reg_default[i] == AW96103_REG_IRQEN)
			aw96103->hostirqen = aw96103_reg_default[i + 1];
		if (aw96103_reg_default[i] == AW96103_REG_SCANCTRL0)
			aw96103->chan_en = FIELD_GET(AW96103_CHAN_EN_MASK,
					aw96103_reg_default[i + 1]);
	}

	return aw96103_channel_scan_start(aw96103);
}

static int aw96103_cfg_all_loaded(const struct firmware *cont,
		struct aw96103 *aw96103)
{
	if (!cont)
		return -EINVAL;

	struct aw_bin *aw_bin __free(kfree) =
		kzalloc(cont->size + sizeof(*aw_bin), GFP_KERNEL);
	if (!aw_bin)
		return -ENOMEM;

	aw_bin->len = cont->size;
	memcpy(aw_bin->data, cont->data, cont->size);
	aw96103_parsing_bin_file(aw_bin);

	snprintf(aw96103->chip_type, sizeof(aw96103->chip_type), "%s",
			aw_bin->chip_type);

	return aw96103_bin_valid_loaded(aw96103, aw_bin);
}

static void aw96103_cfg_update(const struct firmware *fw, void *data)
{
	struct aw96103 *aw96103 = data;
	int ret, i;

	if (!fw || !fw->data) {
		dev_err(aw96103->dev, "%s: No firmware.\n", __func__);
		return;
	}

	ret = aw96103_cfg_all_loaded(fw, aw96103);
	if (ret)
		ret = aw96103_para_loaded(aw96103);
	release_firmware(fw);

	for (i = 0; i <= aw96103->max_channels; i++) {
		if ((aw96103->chan_en >> i) & 0x01)
			aw96103->channels_arr[i].used = true;
		else
			aw96103->channels_arr[i].used = false;
	}
}

static int aw96103_sw_reset(struct aw96103 *aw96103)
{
	int ret;

	ret = regmap_write(aw96103->regmap, AW96103_REG_RESET, 0);
	msleep(20);

	return ret;
}

static void aw96103_irq_handle(struct iio_dev *indio_dev)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);
	u32 curr_status_val;
	u32 curr_status;
	unsigned char i;
	int ret;

	ret = regmap_read(aw96103->regmap, AW96103_REG_STAT0, &curr_status_val);
	if (ret)
		return;

	/*
	 * Iteratively analyze the interrupt status of different channels,
	 * with each channel having 4 interrupt states.
	 */
	for (i = 0; i < AW_CHANNEL_MAX; i++) {
		curr_status = (((curr_status_val >> (24 + i)) & 0x1)) |
			(((curr_status_val >> (16 + i)) & 0x1) << 1) |
			(((curr_status_val >> (8 + i)) & 0x1) << 2) |
			(((curr_status_val >> i) & 0x1) << 3);

		if (!aw96103->channels_arr[i].used ||
		    (aw96103->channels_arr[i].last_channel_info == curr_status))
			continue;

		switch (curr_status) {
		case FAR:
			iio_push_event(indio_dev,
					IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, i,
						IIO_EV_TYPE_THRESH,
						IIO_EV_DIR_RISING),
					iio_get_time_ns(indio_dev));
			break;
		case TRIGGER_TH0:
		case TRIGGER_TH1:
		case TRIGGER_TH2:
		case TRIGGER_TH3:
			iio_push_event(indio_dev,
					IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, i,
						IIO_EV_TYPE_THRESH,
						IIO_EV_DIR_FALLING),
					iio_get_time_ns(indio_dev));
			break;
		default:
			return;
		}
		aw96103->channels_arr[i].last_channel_info = curr_status;
	}
}

static void aw96103_interrupt_clear(struct iio_dev *indio_dev)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);
	int ret;

	ret = regmap_read(aw96103->regmap, AW96103_REG_IRQSRC,
			&aw96103->irq_status);
	if (ret)
		return;

	aw96103_irq_handle(indio_dev);
}

static irqreturn_t aw96103_irq(int irq, void *data)
{
	struct iio_dev *indio_dev = (struct iio_dev *)data;

	aw96103_interrupt_clear(indio_dev);

	return IRQ_HANDLED;
}

static int aw96103_interrupt_init(struct iio_dev *indio_dev)
{
	struct aw96103 *aw96103 = iio_priv(indio_dev);
	unsigned int irq_status;
	int ret;

	ret = regmap_write(aw96103->regmap, AW96103_REG_IRQEN, 0);
	if (ret)
		return ret;
	ret = regmap_read(aw96103->regmap, AW96103_REG_IRQSRC, &irq_status);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(aw96103->dev, aw96103->i2c->irq,
			NULL, aw96103_irq, IRQF_ONESHOT,
			"aw96103_irq", indio_dev);
	if (ret)
		return ret;

	return regmap_write(aw96103->regmap, AW96103_REG_IRQEN,
			aw96103->hostirqen);
}

static int aw96103_wait_chip_init(struct aw96103 *aw96103)
{
	unsigned int cnt = 20;
	u32 reg_data;
	int ret;

	while (cnt--) {
		ret = regmap_read(aw96103->regmap, AW96103_REG_IRQSRC,
				&reg_data);
		if (ret)
			return ret;

		if (FIELD_GET(AW96103_INITOVERIRQ_MASK, reg_data))
			return 0;
		mdelay(1);
	}

	return -EINVAL;
}

static int aw96103_read_chipid(struct aw96103 *aw96103)
{
	unsigned char cnt = 0;
	u32 reg_val = 0;
	int ret;

	while (cnt < AW_READ_CHIPID_RETRIES) {
		ret = regmap_read(aw96103->regmap, AW96103_REG_CHIPID,
				&reg_val);
		if (ret < 0) {
			cnt++;
			usleep_range(2000, 3000);
		} else {
			reg_val = FIELD_GET(AW96103_CHIPID_MASK, reg_val);
			break;
		}
	}

	if (reg_val != AW96103_CHIP_ID)
		dev_err(aw96103->dev,
				"chipid error, error_id=0x%08X\n", reg_val);

	return 0;
}

static int aw96103_version_init(struct aw96103 *aw96103)
{
	u32 fw_ver;
	int ret;

	ret = regmap_read(aw96103->regmap, AW96103_REG_FWVER2, &fw_ver);
	if (ret)
		return ret;
	if (fw_ver == AW_CHIP_AW96103A)
		aw96103->vers = AW96103A;
	else
		aw96103->vers = AW96103;

	return 0;
}

static int aw96103_i2c_probe(struct i2c_client *i2c)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(i2c);
	enum aw96103_sensor_type sensor_type;
	struct iio_dev *aw_iio_dev;
	struct aw96103 *aw96103;
	int ret;

	aw_iio_dev = devm_iio_device_alloc(&i2c->dev, sizeof(*aw96103));
	if (!aw_iio_dev)
		return -ENOMEM;

	aw96103 = iio_priv(aw_iio_dev);
	aw96103->dev = &i2c->dev;
	aw96103->i2c = i2c;
	mutex_init(&aw96103->mutex);
	i2c_set_clientdata(i2c, aw96103);

	sensor_type = (enum aw96103_sensor_type)id->driver_data;
	switch (sensor_type) {
	case AW96103_VAL:
		aw96103->max_channels = AW_CHANNEL3;
		break;
	case AW96105_VAL:
		aw96103->max_channels = AW_CHANNEL5;
		break;
	default:
		return -EINVAL;
	}

	aw96103->regmap = devm_regmap_init_i2c(i2c, &aw96103_regmap_confg);
	if (IS_ERR(aw96103->regmap))
		return PTR_ERR(aw96103->regmap);

	ret = devm_regulator_get_enable(aw96103->dev, "vcc");
	if (ret < 0)
		return ret;

	ret = aw96103_read_chipid(aw96103);
	if (ret)
		return ret;

	ret = aw96103_sw_reset(aw96103);
	if (ret)
		return ret;

	ret = aw96103_wait_chip_init(aw96103);
	if (ret)
		return ret;

	ret = aw96103_version_init(aw96103);
	if (ret)
		return ret;

	ret = request_firmware_nowait(THIS_MODULE, true, "aw96103_0.bin",
			aw96103->dev, GFP_KERNEL, aw96103, aw96103_cfg_update);
	if (ret)
		return ret;

	ret = aw96103_interrupt_init(aw_iio_dev);
	if (ret)
		return ret;

	return aw96103_iio_init(aw_iio_dev);
}

static const struct of_device_id aw96103_dt_match[] = {
	{ .compatible = "awinic,aw96103", (void *)AW96103_VAL },
	{ .compatible = "awinic,aw96105", (void *)AW96105_VAL },
	{ }
};
MODULE_DEVICE_TABLE(of, aw96103_dt_match);

static const struct i2c_device_id aw96103_i2c_id[] = {
	{ "aw96103", AW96103_VAL },
	{ "aw96105", AW96105_VAL },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw96103_i2c_id);

static struct i2c_driver aw96103_i2c_driver = {
	.driver = {
		.name = "aw96103_sensor",
		.of_match_table = aw96103_dt_match,
	},
	.probe = aw96103_i2c_probe,
	.id_table = aw96103_i2c_id,
};
module_i2c_driver(aw96103_i2c_driver);

MODULE_AUTHOR("Wang Shuaijie <wangshuaijie@awinic.com>");
MODULE_DESCRIPTION("Driver for Awinic AW96103 proximity sensor");
MODULE_LICENSE("GPL v2");
