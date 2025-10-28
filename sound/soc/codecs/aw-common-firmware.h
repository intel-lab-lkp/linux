// SPDX-License-Identifier: GPL-2.0-only
//
// aw-common-firmware.h --  awinic amp common firmware interface
//
// Copyright (c) 2025 AWINIC Technology CO., LTD
//
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#ifndef __AW_COMMON_FIRMWARE_H__
#define __AW_COMMON_FIRMWARE_H__

#include "aw-common-device.h"

#define CHECK_REGISTER_NUM_OFFSET	(4)
#define VALID_DATA_LEN			(4)
#define VALID_DATA_ADDR		(4)
#define PARSE_DSP_REG_NUM		(4)
#define REG_DATA_BYTP_LEN		(8)
#define CHECK_DSP_REG_NUM		(12)
#define DSP_VALID_DATA_LEN		(12)
#define DSP_VALID_DATA_ADDR		(12)
#define PARSE_SOC_APP_NUM		(8)
#define CHECK_SOC_APP_NUM		(12)
#define APP_DOWNLOAD_ADDR		(4)
#define APP_VALID_DATA_LEN		(12)
#define APP_VALID_DATA_ADDR		(12)
#define BIN_NUM_MAX			(100)
#define HEADER_LEN			(60)
#define BIN_DATA_TYPE_OFFSET		(8)
#define DATA_LEN			(44)
#define VALID_DATA_ADDR_OFFSET		(60)
#define START_ADDR_OFFSET		(64)

#define HDADER_LEN			(60)

#define HEADER_VERSION_OFFSET		(4)

#define PROJECT_NAME_MAX		(24)
#define CUSTOMER_NAME_MAX		(16)
#define CFG_VERSION_MAX		(4)
#define DEV_NAME_MAX			(16)
#define PROFILE_STR_MAX		(32)

#define ACF_FILE_ID			(0xa15f908)

enum bin_header_version_enum {
	HEADER_VERSION_V1 = 0x01000000,
};

enum data_type_enum {
	DATA_TYPE_REGISTER   = 0x00000000,
	DATA_TYPE_DSP_REG    = 0x00000010,
	DATA_TYPE_DSP_CFG    = 0x00000011,
	DATA_TYPE_SOC_REG    = 0x00000020,
	DATA_TYPE_SOC_APP    = 0x00000021,
	DATA_TYPE_DSP_FW     = 0x00000022,
	DATA_TYPE_MULTI_BINS = 0x00002000,
};

enum data_version_enum {
	DATA_VERSION_V1 = 0x00000001,
	DATA_VERSION_MAX,
};

enum aw_sec_type {
	ACF_SEC_TYPE_REG = 0,
	ACF_SEC_TYPE_DSP,
	ACF_SEC_TYPE_DSP_CFG,
	ACF_SEC_TYPE_DSP_FW,
	ACF_SEC_TYPE_HDR_REG,
	ACF_SEC_TYPE_HDR_DSP_CFG,
	ACF_SEC_TYPE_HDR_DSP_FW,
	ACF_SEC_TYPE_MULTIPLE_BIN,
	ACF_SEC_TYPE_SKT_PROJECT,
	ACF_SEC_TYPE_DSP_PROJECT,
	ACF_SEC_TYPE_MONITOR,
	ACF_SEC_TYPE_MAX,
};

enum aw_prof_type {
	AW_PROFILE_MUSIC = 0,
	AW_PROFILE_VOICE,
	AW_PROFILE_VOIP,
	AW_PROFILE_RINGTONE,
	AW_PROFILE_RINGTONE_HS,
	AW_PROFILE_LOWPOWER,
	AW_PROFILE_BYPASS,
	AW_PROFILE_MMI,
	AW_PROFILE_FM,
	AW_PROFILE_NOTIFICATION,
	AW_PROFILE_RECEIVER,
	AW_PROFILE_MAX,
};

enum profile_data_type {
	AW_DATA_TYPE_REG = 0,
	AW_DATA_TYPE_DSP_CFG,
	AW_DATA_TYPE_DSP_FW,
	AW_DATA_TYPE_MAX,
};

enum {
	AW_DEV_TYPE_OK = 0,
	AW_DEV_TYPE_NONE = 1,
};

enum aw_cfg_dde_type {
	AW_DEV_NONE_TYPE_ID	= 0xFFFFFFFF,
	AW_DEV_TYPE_ID		= 0x00000000,
	AW_SKT_TYPE_ID		= 0x00000001,
	AW_DEV_DEFAULT_TYPE_ID	= 0x00000002,
};

enum aw_profile_status {
	AW_PROFILE_WAIT = 0,
	AW_PROFILE_OK,
};

enum aw_cfg_hdr_version {
	AW_CFG_HDR_VER = 0x00000001,
	AW_CFG_HDR_VER_V1 = 0x01000000,
};

struct aw_cfg_hdr {
	u32 id;
	char project[PROJECT_NAME_MAX];
	char custom[CUSTOMER_NAME_MAX];
	char version[CFG_VERSION_MAX];
	u32 author_id;
	u32 ddt_size;
	u32 ddt_num;
	u32 hdr_offset;
	u32 hdr_version;
	u32 reserved[3];
};

struct aw_cfg_dde {
	u32 type;
	char dev_name[DEV_NAME_MAX];
	u16 dev_index;
	u16 dev_bus;
	u16 dev_addr;
	u16 dev_profile;
	u32 data_type;
	u32 data_size;
	u32 data_offset;
	u32 data_crc;
	u32 reserved[5];
};

struct aw_cfg_dde_v1 {
	u32 type;
	char dev_name[DEV_NAME_MAX];
	u16 dev_index;
	u16 dev_bus;
	u16 dev_addr;
	u16 dev_profile;
	u32 data_type;
	u32 data_size;
	u32 data_offset;
	u32 data_crc;
	char dev_profile_str[PROFILE_STR_MAX];
	u32 chip_id;
	u32 reserved[4];
};

struct bin_header_info {
	unsigned int check_sum;
	unsigned int header_ver;
	unsigned int bin_data_type;
	unsigned int bin_data_ver;
	unsigned int bin_data_len;
	unsigned int ui_ver;
	unsigned char chip_type[8];
	unsigned int reg_byte_len;
	unsigned int data_byte_len;
	unsigned int device_addr;
	unsigned int valid_data_len;
	unsigned int valid_data_addr;

	unsigned int reg_num;
	unsigned int reg_data_byte_len;
	unsigned int download_addr;
	unsigned int app_version;
	unsigned int header_len;
};

struct aw_container {
	int len;
	u8 data[];
};

struct bin_container {
	unsigned int len;
	unsigned char data[];
};

struct aw_bin {
	unsigned char *p_addr;
	unsigned int all_bin_parse_num;
	unsigned int multi_bin_parse_num;
	unsigned int single_bin_parse_num;
	struct bin_header_info header_info[BIN_NUM_MAX];
	struct bin_container info;
};

int aw_dev_load_acf_check(struct aw_device *aw_dev, struct aw_container *aw_cfg);
int aw_dev_cfg_load(struct aw_device *aw_dev, struct aw_container *aw_cfg);

#endif
