// SPDX-License-Identifier: GPL-2.0-only
//
// aw-dev-common.h --  awinic amp common driver interface
//
// Copyright (c) 2025 AWINIC Technology CO., LTD
//
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#include <linux/crc8.h>
#include <linux/i2c.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/regmap.h>
#include "aw-common-device.h"
#include "aw-common-firmware.h"

static int aw_dev_dsp_write_16bit(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int dsp_data)
{
	int ret;

	ret = regmap_write(aw_dev->regmap, AW_DSPMADD_REG, dsp_addr);
	if (ret) {
		dev_err(aw_dev->dev, "%s write addr error, ret=%d", __func__, ret);
		return ret;
	}

	ret = regmap_write(aw_dev->regmap, AW_DSPMDAT_REG, (u16)dsp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s write data error, ret=%d", __func__, ret);
		return ret;
	}

	return 0;
}

static int aw_dev_dsp_write_32bit(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int dsp_data)
{
	u16 temp_data;
	int ret;

	ret = regmap_write(aw_dev->regmap, AW_DSPMADD_REG, dsp_addr);
	if (ret) {
		dev_err(aw_dev->dev, "%s write addr error, ret=%d", __func__, ret);
		return ret;
	}

	temp_data = dsp_data & AW_DSP_16_DATA_MASK;
	ret = regmap_write(aw_dev->regmap, AW_DSPMDAT_REG, (u16)temp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s write datal error, ret=%d", __func__, ret);
		return ret;
	}

	temp_data = dsp_data >> 16;
	ret = regmap_write(aw_dev->regmap, AW_DSPMDAT_REG, (u16)temp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s write datah error, ret=%d", __func__, ret);
		return ret;
	}

	return 0;
}

int aw_dev_dsp_write(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int dsp_data, unsigned char data_type)
{
	u32 reg_value;
	int ret;

	mutex_lock(&aw_dev->dsp_lock);
	switch (data_type) {
	case AW_DSP_16_DATA:
		ret = aw_dev_dsp_write_16bit(aw_dev, dsp_addr, dsp_data);
		if (ret)
			dev_err(aw_dev->dev, "write dsp_addr[0x%x] 16-bit dsp_data[0x%x] failed",
					dsp_addr, dsp_data);
		break;
	case AW_DSP_32_DATA:
		ret = aw_dev_dsp_write_32bit(aw_dev, dsp_addr, dsp_data);
		if (ret)
			dev_err(aw_dev->dev, "write dsp_addr[0x%x] 32-bit dsp_data[0x%x] failed",
					dsp_addr, dsp_data);
		break;
	default:
		dev_err(aw_dev->dev, "data type[%d] unsupported", data_type);
		ret = -EINVAL;
		break;
	}

	/* clear dsp chip select state */
	if (regmap_read(aw_dev->regmap, AW_ID_REG, &reg_value))
		dev_err(aw_dev->dev, "%s fail to clear chip state. Err=%d\n", __func__, ret);
	mutex_unlock(&aw_dev->dsp_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(aw_dev_dsp_write);

int aw_dev_dsp_read_16bit(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int *dsp_data)
{
	unsigned int temp_data;
	int ret;

	ret = regmap_write(aw_dev->regmap, AW_DSPMADD_REG, dsp_addr);
	if (ret) {
		dev_err(aw_dev->dev, "%s write error, ret=%d", __func__, ret);
		return ret;
	}

	ret = regmap_read(aw_dev->regmap, AW_DSPMDAT_REG, &temp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s read error, ret=%d", __func__, ret);
		return ret;
	}
	*dsp_data = temp_data;

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_dsp_read_16bit);

static int aw_dev_dsp_read_32bit(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int *dsp_data)
{
	unsigned int temp_data;
	int ret;

	ret = regmap_write(aw_dev->regmap, AW_DSPMADD_REG, dsp_addr);
	if (ret) {
		dev_err(aw_dev->dev, "%s write error, ret=%d", __func__, ret);
		return ret;
	}

	ret = regmap_read(aw_dev->regmap, AW_DSPMDAT_REG, &temp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s read error, ret=%d", __func__, ret);
		return ret;
	}
	*dsp_data = temp_data;

	ret = regmap_read(aw_dev->regmap, AW_DSPMDAT_REG, &temp_data);
	if (ret) {
		dev_err(aw_dev->dev, "%s read error, ret=%d", __func__, ret);
		return ret;
	}
	*dsp_data |= (temp_data << 16);

	return 0;
}

int aw_dev_dsp_read(struct aw_device *aw_dev,
		unsigned short dsp_addr, unsigned int *dsp_data, unsigned char data_type)
{
	u32 reg_value;
	int ret;

	mutex_lock(&aw_dev->dsp_lock);
	switch (data_type) {
	case AW_DSP_16_DATA:
		ret = aw_dev_dsp_read_16bit(aw_dev, dsp_addr, dsp_data);
		if (ret)
			dev_err(aw_dev->dev, "read dsp_addr[0x%x] 16-bit dsp_data[0x%x] failed",
					dsp_addr, *dsp_data);
		break;
	case AW_DSP_32_DATA:
		ret = aw_dev_dsp_read_32bit(aw_dev, dsp_addr, dsp_data);
		if (ret)
			dev_err(aw_dev->dev, "read dsp_addr[0x%x] 32r-bit dsp_data[0x%x] failed",
					dsp_addr, *dsp_data);
		break;
	default:
		dev_err(aw_dev->dev, "data type[%d] unsupported", data_type);
		ret = -EINVAL;
		break;
	}

	/* clear dsp chip select state */
	if (regmap_read(aw_dev->regmap, AW_ID_REG, &reg_value))
		dev_err(aw_dev->dev, "%s fail to clear chip state. Err=%d\n", __func__, ret);
	mutex_unlock(&aw_dev->dsp_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(aw_dev_dsp_read);

int aw_dev_get_dsp_status(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW_WDT_REG, &reg_val);
	if (ret)
		return ret;
	if (!(reg_val & (~AW_WDT_CNT_MASK)))
		ret = -EPERM;

	return ret;
}
EXPORT_SYMBOL_GPL(aw_dev_get_dsp_status);

void aw_dev_get_int_status(struct aw_device *aw_dev, unsigned short *int_status)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW_SYSINT_REG, &reg_val);
	if (ret)
		dev_err(aw_dev->dev, "read interrupt reg fail, ret=%d", ret);
	else
		*int_status = reg_val;

	dev_dbg(aw_dev->dev, "read interrupt reg = 0x%04x", *int_status);
}
EXPORT_SYMBOL_GPL(aw_dev_get_int_status);

void aw_dev_clear_int_status(struct aw_device *aw_dev)
{
	u16 int_status;

	/* read int status and clear */
	aw_dev_get_int_status(aw_dev, &int_status);
	/* make sure int status is clear */
	aw_dev_get_int_status(aw_dev, &int_status);
	if (int_status)
		dev_info(aw_dev->dev, "int status(%d) is not cleaned.\n", int_status);
}
EXPORT_SYMBOL_GPL(aw_dev_clear_int_status);

static int aw_dev_dsp_update_container(struct aw_device *aw_dev,
			unsigned char *data, unsigned int len, unsigned short base)
{
	u32 tmp_len;
	int i, ret;

	mutex_lock(&aw_dev->dsp_lock);
	ret = regmap_write(aw_dev->regmap, AW_DSPMADD_REG, base);
	if (ret)
		goto error_operation;

	for (i = 0; i < len; i += AW_MAX_RAM_WRITE_BYTE_SIZE) {
		tmp_len = min(len - i, AW_MAX_RAM_WRITE_BYTE_SIZE);
		ret = regmap_raw_write(aw_dev->regmap, AW_DSPMDAT_REG,
					&data[i], tmp_len);
		if (ret)
			goto error_operation;
	}
	mutex_unlock(&aw_dev->dsp_lock);

	return 0;

error_operation:
	mutex_unlock(&aw_dev->dsp_lock);
	return ret;
}

int aw_dev_dsp_update_fw(struct aw_device *aw_dev,
			unsigned char *data, unsigned int len, unsigned int addr)
{
	dev_dbg(aw_dev->dev, "dsp firmware len:%d", len);

	if (!len || !data) {
		dev_err(aw_dev->dev, "dsp firmware data is null or len is 0");
		return -EINVAL;
	}
	aw_dev_dsp_update_container(aw_dev, data, len, addr);
	aw_dev->dsp_fw_len = len;

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_dsp_update_fw);

int aw_dev_dsp_update_cfg(struct aw_device *aw_dev,
			unsigned char *data, unsigned int len, unsigned int addr)
{
	dev_dbg(aw_dev->dev, "dsp config len:%d", len);

	if (!len || !data) {
		dev_err(aw_dev->dev, "dsp config data is null or len is 0");
		return -EINVAL;
	}

	aw_dev_dsp_update_container(aw_dev, data, len, addr);
	aw_dev->dsp_cfg_len = len;

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_dsp_update_cfg);

int aw_dev_set_profile_index(struct aw_device *aw_dev, int index)
{
	/* check the index whether is valid */
	if ((index >= aw_dev->prof_info.count) || (index < 0))
		return -EINVAL;
	/* check the index whether change */
	if (aw_dev->prof_index == index)
		return -EINVAL;

	aw_dev->prof_index = index;
	dev_dbg(aw_dev->dev, "set prof[%s]",
		aw_dev->prof_info.prof_name_list[aw_dev->prof_info.prof_desc[index].id]);

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_set_profile_index);

int aw_dev_get_prof_name(struct aw_device *aw_dev, int index, char **prof_name)
{
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	struct aw_prof_desc *prof_desc;

	if ((index >= aw_dev->prof_info.count) || (index < 0)) {
		dev_err(aw_dev->dev, "index[%d] overflow count[%d]",
			index, aw_dev->prof_info.count);
		return -EINVAL;
	}

	prof_desc = &aw_dev->prof_info.prof_desc[index];

	*prof_name = prof_info->prof_name_list[prof_desc->id];

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_get_prof_name);

int aw_dev_get_prof_data(struct aw_device *aw_dev, int index,
			struct aw_prof_desc **prof_desc)
{
	if ((index >= aw_dev->prof_info.count) || (index < 0)) {
		dev_err(aw_dev->dev, "%s: index[%d] overflow count[%d]\n",
				__func__, index, aw_dev->prof_info.count);
		return -EINVAL;
	}

	*prof_desc = &aw_dev->prof_info.prof_desc[index];

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_get_prof_data);

static int aw_dev_read_chipid(struct aw_device *aw_dev, u16 *chip_id)
{
	int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW_ID_REG, &reg_val);
	if (ret) {
		dev_err(aw_dev->dev, "%s read chipid error. ret = %d", __func__, ret);
		return ret;
	}

	dev_info(aw_dev->dev, "chip id = %x\n", reg_val);
	*chip_id = reg_val;

	return 0;
}

static void aw_parse_channel_dt(struct aw_device *aw_dev)
{
	struct device_node *np = aw_dev->dev->of_node;
	u32 channel_value;

	of_property_read_u32(np, "awinic,audio-channel", &channel_value);
	aw_dev->channel = channel_value;
	aw_dev->phase_sync = of_property_read_bool(np, "awinic,sync-flag");
}

int aw_dev_init(struct aw_device *aw_dev, struct i2c_client *i2c, struct regmap *regmap)
{
	u16 chip_id;
	int ret;

	aw_dev->i2c = i2c;
	aw_dev->dev = &i2c->dev;
	aw_dev->regmap = regmap;
	mutex_init(&aw_dev->dsp_lock);

	/* read chip id */
	ret = aw_dev_read_chipid(aw_dev, &chip_id);
	if (ret) {
		dev_err(&i2c->dev, "dev_read_chipid failed ret=%d", ret);
		return ret;
	}

	aw_dev->chip_id = chip_id;
	aw_dev->acf = NULL;
	aw_dev->prof_info.prof_desc = NULL;
	aw_dev->prof_info.count = 0;
	aw_dev->prof_info.prof_type = AW_DEV_NONE_TYPE_ID;
	aw_dev->channel = 0;
	aw_dev->fw_status = AW_DEV_FW_FAILED;

	aw_dev->fade_step = AW_VOLUME_STEP_DB;
	aw_dev->volume_desc.ctl_volume = AW_VOL_DEFAULT_VALUE;

	aw_dev->fade_in_time = AW_1000_US / 10;
	aw_dev->fade_out_time = AW_1000_US >> 1;

	aw_parse_channel_dt(aw_dev);

	return 0;
}
EXPORT_SYMBOL_GPL(aw_dev_init);

int aw_dev_request_firmware_file(struct aw_device *aw_dev, char *acf_name)
{
	const struct firmware *cont = NULL;
	int ret;

	aw_dev->fw_status = AW_DEV_FW_FAILED;

	ret = request_firmware(&cont, acf_name, aw_dev->dev);
	if ((ret < 0) || (!cont)) {
		dev_err(aw_dev->dev, "load [%s] failed!", acf_name);
		return ret;
	}

	dev_dbg(aw_dev->dev, "loaded %s - size: %zu\n", acf_name, cont ? cont->size : 0);

	aw_dev->aw_cfg = devm_kzalloc(aw_dev->dev, cont->size + sizeof(int), GFP_KERNEL);
	if (!aw_dev->aw_cfg) {
		release_firmware(cont);
		return -ENOMEM;
	}

	aw_dev->aw_cfg->len = (int)cont->size;
	memcpy(aw_dev->aw_cfg->data, cont->data, cont->size);
	release_firmware(cont);

	ret = aw_dev_load_acf_check(aw_dev, aw_dev->aw_cfg);
	if (ret < 0) {
		dev_err(aw_dev->dev, "Load [%s] failed ....!", acf_name);
		return ret;
	}

	ret = aw_dev_cfg_load(aw_dev, aw_dev->aw_cfg);
	if (ret) {
		dev_err(aw_dev->dev, "aw_dev acf parse failed");
		return -EINVAL;
	}

	aw_dev->prof_cur = aw_dev->prof_info.prof_desc[0].id;
	aw_dev->prof_index = aw_dev->prof_info.prof_desc[0].id;

	return ret;
}
EXPORT_SYMBOL_GPL(aw_dev_request_firmware_file);

void aw_dev_fade_in(struct aw_device *aw_dev,
	void (*set_volume)(struct aw_device *aw_dev, unsigned int value))
{
	struct aw_volume_desc *desc = &aw_dev->volume_desc;
	u16 fade_in_vol = desc->ctl_volume;
	int fade_step = aw_dev->fade_step;
	int i;

	if (fade_step == 0 || aw_dev->fade_in_time == 0) {
		set_volume(aw_dev, fade_in_vol);
		return;
	}

	for (i = desc->mute_volume; i >= fade_in_vol; i -= fade_step) {
		set_volume(aw_dev, i);
		usleep_range(aw_dev->fade_in_time, aw_dev->fade_in_time + 10);
	}

	if (i != fade_in_vol)
		set_volume(aw_dev, fade_in_vol);
}
EXPORT_SYMBOL_GPL(aw_dev_fade_in);

void aw_dev_fade_out(struct aw_device *aw_dev,
	void (*set_volume)(struct aw_device *aw_dev, unsigned int value))
{
	struct aw_volume_desc *desc = &aw_dev->volume_desc;
	int fade_step = aw_dev->fade_step;
	int i;

	if (fade_step == 0 || aw_dev->fade_out_time == 0) {
		set_volume(aw_dev, desc->mute_volume);
		return;
	}

	for (i = desc->ctl_volume; i <= desc->mute_volume; i += fade_step) {
		set_volume(aw_dev, i);
		usleep_range(aw_dev->fade_out_time, aw_dev->fade_out_time + 10);
	}

	if (i != desc->mute_volume) {
		set_volume(aw_dev, desc->mute_volume);
		usleep_range(aw_dev->fade_out_time, aw_dev->fade_out_time + 10);
	}
}
EXPORT_SYMBOL_GPL(aw_dev_fade_out);

void aw_hw_reset(struct aw_device *aw_dev)
{
	gpiod_set_value_cansleep(aw_dev->reset_gpio, 1);
	usleep_range(AW_1000_US, AW_1000_US + 10);
	gpiod_set_value_cansleep(aw_dev->reset_gpio, 0);
	usleep_range(AW_1000_US, AW_1000_US + 10);
}
EXPORT_SYMBOL_GPL(aw_hw_reset);

MODULE_DESCRIPTION("awinic device lib");
MODULE_LICENSE("GPL v2");
