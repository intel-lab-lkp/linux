// SPDX-License-Identifier: GPL-2.0-only
//
// aw88395_lib.c  -- ACF bin parsing and check library file for aw88395
//
// Copyright (c) 2022-2023 AWINIC Technology CO., LTD
//
// Author: Bruce zhao <zhaolei@awinic.com>
//

#include <linux/crc8.h>
#include <linux/i2c.h>
#include "aw88395_lib.h"
#include "aw88395_device.h"
#include "aw88395_reg.h"

#define AW88395_CRC8_POLYNOMIAL 0x8C
DECLARE_CRC8_TABLE(aw_crc8_table);

static char *profile_name[AW88395_PROFILE_MAX] = {
	"Music", "Voice", "Voip", "Ringtone",
	"Ringtone_hs", "Lowpower", "Bypass",
	"Mmi", "Fm", "Notification", "Receiver"
};

static int aw_parse_bin_header(struct aw_device *aw_dev, struct aw_bin *bin);

static void aw_get_single_bin_header(struct aw_bin *bin)
{
	memcpy((void *)&bin->header_info[bin->all_bin_parse_num], bin->p_addr, DATA_LEN);

	bin->header_info[bin->all_bin_parse_num].header_len = HEADER_LEN;
	bin->all_bin_parse_num += 1;
}

static int aw_parse_one_of_multi_bins(struct aw_device *aw_dev, unsigned int bin_num,
					int bin_serial_num, struct aw_bin *bin)
{
	struct bin_header_info aw_bin_header_info;
	unsigned int bin_start_addr;
	unsigned int valid_data_len;

	if (bin->info.len < sizeof(struct bin_header_info)) {
		dev_err(aw_dev->dev, "bin_header_info size[%d] overflow file size[%d]\n",
				(int)sizeof(struct bin_header_info), bin->info.len);
		return -EINVAL;
	}

	aw_bin_header_info = bin->header_info[bin->all_bin_parse_num - 1];
	if (!bin_serial_num) {
		bin_start_addr = le32_to_cpup((void *)(bin->p_addr + START_ADDR_OFFSET));
		bin->p_addr += (HEADER_LEN + bin_start_addr);
		bin->header_info[bin->all_bin_parse_num].valid_data_addr =
			aw_bin_header_info.valid_data_addr + VALID_DATA_ADDR + 8 * bin_num +
			VALID_DATA_ADDR_OFFSET;
	} else {
		valid_data_len = aw_bin_header_info.bin_data_len;
		bin->p_addr += (HDADER_LEN + valid_data_len);
		bin->header_info[bin->all_bin_parse_num].valid_data_addr =
		    aw_bin_header_info.valid_data_addr + aw_bin_header_info.bin_data_len +
		    VALID_DATA_ADDR_OFFSET;
	}

	return aw_parse_bin_header(aw_dev, bin);
}

static int aw_get_multi_bin_header(struct aw_device *aw_dev, struct aw_bin *bin)
{
	unsigned int bin_num, i;
	int ret;

	bin_num = le32_to_cpup((void *)(bin->p_addr + VALID_DATA_ADDR_OFFSET));
	if (bin->multi_bin_parse_num == 1)
		bin->header_info[bin->all_bin_parse_num].valid_data_addr =
							VALID_DATA_ADDR_OFFSET;

	aw_get_single_bin_header(bin);

	for (i = 0; i < bin_num; i++) {
		dev_dbg(aw_dev->dev, "aw_bin_parse enter multi bin for is %d\n", i);
		ret = aw_parse_one_of_multi_bins(aw_dev, bin_num, i, bin);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int aw_parse_bin_header(struct aw_device *aw_dev, struct aw_bin *bin)
{
	unsigned int bin_data_type;

	if (bin->info.len < sizeof(struct bin_header_info)) {
		dev_err(aw_dev->dev, "bin_header_info size[%d] overflow file size[%d]\n",
				(int)sizeof(struct bin_header_info), bin->info.len);
		return -EINVAL;
	}

	bin_data_type = le32_to_cpup((void *)(bin->p_addr + BIN_DATA_TYPE_OFFSET));
	dev_dbg(aw_dev->dev, "aw_bin_parse bin_data_type 0x%x\n", bin_data_type);
	switch (bin_data_type) {
	case DATA_TYPE_REGISTER:
	case DATA_TYPE_DSP_REG:
	case DATA_TYPE_SOC_APP:
		bin->single_bin_parse_num += 1;
		dev_dbg(aw_dev->dev, "%s bin->single_bin_parse_num is %d\n", __func__,
						bin->single_bin_parse_num);
		if (!bin->multi_bin_parse_num)
			bin->header_info[bin->all_bin_parse_num].valid_data_addr =
								VALID_DATA_ADDR_OFFSET;
		aw_get_single_bin_header(bin);
		return 0;
	case DATA_TYPE_MULTI_BINS:
		bin->multi_bin_parse_num += 1;
		dev_dbg(aw_dev->dev, "%s bin->multi_bin_parse_num is %d\n", __func__,
						bin->multi_bin_parse_num);
		return aw_get_multi_bin_header(aw_dev, bin);
	default:
		dev_dbg(aw_dev->dev, "%s There is no corresponding type\n", __func__);
		return 0;
	}
}

static int aw88261_dev_cfg_get_valid_prof(struct aw_device *aw_dev,
				struct aw_all_prof_info all_prof_info)
{
	struct aw_prof_desc *prof_desc = all_prof_info.prof_desc;
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	int num = 0;
	int i;

	for (i = 0; i < AW88395_PROFILE_MAX; i++) {
		if (prof_desc[i].prof_st == AW88395_PROFILE_OK)
			prof_info->count++;
	}

	dev_dbg(aw_dev->dev, "get valid profile:%d", aw_dev->prof_info.count);

	if (!prof_info->count) {
		dev_err(aw_dev->dev, "no profile data");
		return -EPERM;
	}

	prof_info->prof_desc = devm_kcalloc(aw_dev->dev,
					prof_info->count, sizeof(struct aw_prof_desc),
					GFP_KERNEL);
	if (!prof_info->prof_desc)
		return -ENOMEM;

	for (i = 0; i < AW88395_PROFILE_MAX; i++) {
		if (prof_desc[i].prof_st == AW88395_PROFILE_OK) {
			if (num >= prof_info->count) {
				dev_err(aw_dev->dev, "overflow count[%d]",
						prof_info->count);
				return -EINVAL;
			}
			prof_info->prof_desc[num] = prof_desc[i];
			prof_info->prof_desc[num].id = i;
			num++;
		}
	}

	return 0;
}

static int aw88395_dev_cfg_get_valid_prof(struct aw_device *aw_dev,
				struct aw_all_prof_info all_prof_info)
{
	struct aw_prof_desc *prof_desc = all_prof_info.prof_desc;
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	struct aw_sec_data_desc *sec_desc;
	int num = 0;
	int i;

	for (i = 0; i < AW88395_PROFILE_MAX; i++) {
		if (prof_desc[i].prof_st == AW88395_PROFILE_OK) {
			sec_desc = prof_desc[i].sec_desc;
			if ((sec_desc[AW88395_DATA_TYPE_REG].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_REG].len != 0) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_CFG].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_CFG].len != 0) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_FW].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_FW].len != 0))
				prof_info->count++;
		}
	}

	dev_dbg(aw_dev->dev, "get valid profile:%d", aw_dev->prof_info.count);

	if (!prof_info->count) {
		dev_err(aw_dev->dev, "no profile data");
		return -EPERM;
	}

	prof_info->prof_desc = devm_kcalloc(aw_dev->dev,
					prof_info->count, sizeof(struct aw_prof_desc),
					GFP_KERNEL);
	if (!prof_info->prof_desc)
		return -ENOMEM;

	for (i = 0; i < AW88395_PROFILE_MAX; i++) {
		if (prof_desc[i].prof_st == AW88395_PROFILE_OK) {
			sec_desc = prof_desc[i].sec_desc;
			if ((sec_desc[AW88395_DATA_TYPE_REG].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_REG].len != 0) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_CFG].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_CFG].len != 0) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_FW].data != NULL) &&
			    (sec_desc[AW88395_DATA_TYPE_DSP_FW].len != 0)) {
				if (num >= prof_info->count) {
					dev_err(aw_dev->dev, "overflow count[%d]",
							prof_info->count);
					return -EINVAL;
				}
				prof_info->prof_desc[num] = prof_desc[i];
				prof_info->prof_desc[num].id = i;
				num++;
			}
		}
	}

	return 0;
}

static int aw_dev_load_cfg_by_hdr(struct aw_device *aw_dev,
		struct aw_cfg_hdr *prof_hdr)
{
	struct aw_all_prof_info *all_prof_info;
	int ret;

	all_prof_info = devm_kzalloc(aw_dev->dev, sizeof(struct aw_all_prof_info), GFP_KERNEL);
	if (!all_prof_info)
		return -ENOMEM;

	switch (aw_dev->chip_id) {
	case AW88395_CHIP_ID:
		ret = aw88395_dev_cfg_get_valid_prof(aw_dev, *all_prof_info);
		if (ret < 0)
			goto exit;
		break;
	case AW88261_CHIP_ID:
		ret = aw88261_dev_cfg_get_valid_prof(aw_dev, *all_prof_info);
		if (ret < 0)
			goto exit;
		break;
	default:
		dev_err(aw_dev->dev, "valid prof unsupported");
		ret = -EINVAL;
		break;
	}

	aw_dev->prof_info.prof_name_list = profile_name;

exit:
	devm_kfree(aw_dev->dev, all_prof_info);
	return ret;
}

static int aw_dev_create_prof_name_list_v1(struct aw_device *aw_dev)
{
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	struct aw_prof_desc *prof_desc = prof_info->prof_desc;
	int i;

	if (!prof_desc) {
		dev_err(aw_dev->dev, "prof_desc is NULL");
		return -EINVAL;
	}

	prof_info->prof_name_list = devm_kzalloc(aw_dev->dev,
					prof_info->count * PROFILE_STR_MAX,
					GFP_KERNEL);
	if (!prof_info->prof_name_list)
		return -ENOMEM;

	for (i = 0; i < prof_info->count; i++) {
		prof_desc[i].id = i;
		prof_info->prof_name_list[i] = prof_desc[i].prf_str;
		dev_dbg(aw_dev->dev, "prof name is %s", prof_info->prof_name_list[i]);
	}

	return 0;
}

static int aw_dev_load_cfg_by_hdr_v1(struct aw_device *aw_dev,
						struct aw_container *aw_cfg)
{
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	int ret;

	prof_info->prof_desc = devm_kcalloc(aw_dev->dev,
					prof_info->count, sizeof(struct aw_prof_desc),
					GFP_KERNEL);
	if (!prof_info->prof_desc)
		return -ENOMEM;

	ret = aw_dev_create_prof_name_list_v1(aw_dev);
	if (ret < 0) {
		dev_err(aw_dev->dev, "create prof name list failed");
		return ret;
	}

	return 0;
}

int aw88395_dev_cfg_load(struct aw_device *aw_dev, struct aw_container *aw_cfg)
{
	struct aw_cfg_hdr *cfg_hdr;
	int ret;

	cfg_hdr = (struct aw_cfg_hdr *)aw_cfg->data;

	switch (cfg_hdr->hdr_version) {
	case AW88395_CFG_HDR_VER:
		ret = aw_dev_load_cfg_by_hdr(aw_dev, cfg_hdr);
		if (ret < 0) {
			dev_err(aw_dev->dev, "hdr_version[0x%x] parse failed",
						cfg_hdr->hdr_version);
			return ret;
		}
		break;
	case AW88395_CFG_HDR_VER_V1:
		ret = aw_dev_load_cfg_by_hdr_v1(aw_dev, aw_cfg);
		if (ret < 0) {
			dev_err(aw_dev->dev, "hdr_version[0x%x] parse failed",
						cfg_hdr->hdr_version);
			return ret;
		}
		break;
	default:
		dev_err(aw_dev->dev, "unsupported hdr_version [0x%x]", cfg_hdr->hdr_version);
		return -EINVAL;
	}
	aw_dev->fw_status = AW88395_DEV_FW_OK;

	return 0;
}
EXPORT_SYMBOL_GPL(aw88395_dev_cfg_load);

static int aw_dev_check_cfg_by_hdr(struct aw_device *aw_dev, struct aw_container *aw_cfg)
{
	struct aw_cfg_hdr *cfg_hdr;
	unsigned int act_data = 0;
	unsigned int hdr_ddt_len;

	cfg_hdr = (struct aw_cfg_hdr *)aw_cfg->data;
	/* check file type id is awinic acf file */
	if (cfg_hdr->id != ACF_FILE_ID) {
		dev_err(aw_dev->dev, "not acf type file");
		return -EINVAL;
	}

	hdr_ddt_len = cfg_hdr->hdr_offset + cfg_hdr->ddt_size;
	if (hdr_ddt_len > aw_cfg->len) {
		dev_err(aw_dev->dev, "hdr_len with ddt_len [%d] overflow file size[%d]",
		cfg_hdr->hdr_offset, aw_cfg->len);
		return -EINVAL;
	}

	/* check data size */
	act_data += hdr_ddt_len;

	if (act_data != aw_cfg->len) {
		dev_err(aw_dev->dev, "act_data[%d] not equal to file size[%d]!",
			act_data, aw_cfg->len);
		return -EINVAL;
	}

	return 0;
}

static int aw_dev_check_acf_by_hdr_v1(struct aw_device *aw_dev, struct aw_container *aw_cfg)
{
	struct aw_cfg_hdr *cfg_hdr;
	unsigned int act_data = 0;
	unsigned int hdr_ddt_len;

	cfg_hdr = (struct aw_cfg_hdr *)aw_cfg->data;

	/* check file type id is awinic acf file */
	if (cfg_hdr->id != ACF_FILE_ID) {
		dev_err(aw_dev->dev, "not acf type file");
		return -EINVAL;
	}

	hdr_ddt_len = cfg_hdr->hdr_offset + cfg_hdr->ddt_size;
	if (hdr_ddt_len > aw_cfg->len) {
		dev_err(aw_dev->dev, "hdrlen with ddt_len [%d] overflow file size[%d]",
		cfg_hdr->hdr_offset, aw_cfg->len);
		return -EINVAL;
	}

	/* check data size */
	act_data += hdr_ddt_len;

	if (act_data != aw_cfg->len) {
		dev_err(aw_dev->dev, "act_data[%d] not equal to file size[%d]!",
			act_data, aw_cfg->len);
		return -EINVAL;
	}

	return 0;
}

int aw88395_dev_load_acf_check(struct aw_device *aw_dev, struct aw_container *aw_cfg)
{
	struct aw_cfg_hdr *cfg_hdr;

	if (!aw_cfg) {
		dev_err(aw_dev->dev, "aw_prof is NULL");
		return -EINVAL;
	}

	if (aw_cfg->len < sizeof(struct aw_cfg_hdr)) {
		dev_err(aw_dev->dev, "cfg hdr size[%d] overflow file size[%d]",
			aw_cfg->len, (int)sizeof(struct aw_cfg_hdr));
		return -EINVAL;
	}

	crc8_populate_lsb(aw_crc8_table, AW88395_CRC8_POLYNOMIAL);

	cfg_hdr = (struct aw_cfg_hdr *)aw_cfg->data;
	switch (cfg_hdr->hdr_version) {
	case AW88395_CFG_HDR_VER:
		return aw_dev_check_cfg_by_hdr(aw_dev, aw_cfg);
	case AW88395_CFG_HDR_VER_V1:
		return aw_dev_check_acf_by_hdr_v1(aw_dev, aw_cfg);
	default:
		dev_err(aw_dev->dev, "unsupported hdr_version [0x%x]", cfg_hdr->hdr_version);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(aw88395_dev_load_acf_check);

MODULE_DESCRIPTION("AW88395 ACF File Parsing Lib");
MODULE_LICENSE("GPL v2");
