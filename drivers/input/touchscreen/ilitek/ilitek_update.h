// SPDX-License-Identifier: GPL-2.0
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#ifndef __ILITEK_UPDATE_H__
#define __ILITEK_UPDATE_H__

#include "ilitek_protocol.h"

#define UPDATE_LEN			1024

#define ILITEK_FW_FILE_SIZE		(512 * 1024)
#define ILITEK_FW_BUF_SIZE		(256 * 1024)

enum fw_file_type {
	fw_hex = 0,
	fw_bin,
	fw_ili,
};

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif

struct __PACKED__ mapping_info_lego {
	u8 tuning_ver[4];
	u8 fw_ver[4];
	u8 core_test;
	u8 core_day;
	u8 core_month;
	u8 core_year;
	u32 core_ver;
	u8 vendor_ver[6];
	u8 _reserve_1[8];
	u16 customer_id;
	u16 fwid;
	u16 i2c_addr;
	u8 _reserve_2[2];
	char model_name[16];
	u8 _reserve_3[2];
	u8 ic_num;
	u8 total_tuning_num;
	u16 sizeof_tuning;
	u16 sizeof_tp_param;
	u16 sizeof_sys_info;
	u16 sizeof_sys_algo;
	u16 sizeof_key_info;
	u8 block_num;
	u8 support_tuning_num;
	u8 _reserve_4[2];

	struct __PACKED__ {
		u8 addr[3];
	} blocks[10];

	u8 _reserve_5[9];
	u8 end_addr[3];
	u8 _reserve_6[2];
};

union __PACKED__ mapping_info {
	struct __PACKED__ {
		u8 mapping_ver[3];
		u8 protocol_ver[3];
		u8 ic_name[6];

		struct mapping_info_lego _lego;
	};
};

/*
 * for V3, "check" is checksum, block[0] for AP and block[1] for Data Flash.
 * for V6, "check" is CRC.
 */
struct __PACKED__ ilitek_block {
	bool check_match;
	u32 start;
	u32 end;
	u32 check;
	u32 offset;
};

struct __PACKED__ ilitek_fw_file_info {
	char ic_name[8];
	u8 fw_ver[8];
	u16 fwid;

	u8 block_num;
	struct ilitek_block blocks[ILTIEK_MAX_BLOCK_NUM];

	u32 mm_addr;
	u32 mm_size;

	u32 buf_size;
	u8 *buf;

	/* fw file's sensor-id or fwid or other id */
	u32 id;
	u8 type;
};

#ifdef _WIN32
#pragma pack()
#endif

/* return file size in # of bytes, or negative error code */
typedef int (*read_fw_t)(WCHAR *, u8 *, int, void *);
/* update progress of fw updating */
typedef void (*update_progress_t)(u8, void *);
/* update fw info to callers */
typedef void (*update_fw_file_info_t)(struct ilitek_fw_file_info *, void *);

/* notify caller before/after slave upgrade (for ITS only) */
typedef void (*slave_update_notify_t)(bool, void *);
/* update fw version and crc/checksum before/after update (for ITS only) */
typedef void (*update_fw_ic_info_t)(bool, u8 *, u32 *, int, void *);

struct ilitek_update_callback {
	read_fw_t read_fw;
	update_progress_t update_progress;
	update_fw_file_info_t update_fw_file_info;

	slave_update_notify_t slave_update_notify;
	update_fw_ic_info_t update_fw_ic_info;
};

enum fw_ver_check_policy {
	allow_fw_ver_downgrade = 1,
	allow_fw_ver_same = 2,
};

struct ilitek_fw_settings {
	s8 retry;
	bool fw_check_only;
	bool force_update;

	bool fw_ver_check;
	u8 fw_ver_policy;
	u8 fw_ver[8];
};

struct ilitek_fw_handle {
	struct ilitek_ts_device *dev;
	void *_private;

	/* upgrade options */
	struct ilitek_fw_settings setting;

	/* common variable */
	int update_len;

	struct ilitek_fw_file_info file;

	/* M3 + M2V */
	bool m2v;
	bool m2v_need_update;
	u8 *m2v_buf;
	u32 m2v_checksum;
	u8 m2v_fw_ver[8];

	/* upgrade status */
	unsigned int progress_curr;
	unsigned int progress_max;
	u8 progress;

	/* callbacks */
	struct ilitek_update_callback cb;
};

#ifdef __cplusplus
extern "C" {
#endif

void __DLL *ilitek_update_init(void *_dev, bool need_update_ts_info,
			       struct ilitek_update_callback *callback,
			       void *_private);

void __DLL ilitek_update_exit(void *handle);

void __DLL ilitek_update_set_data_length(void *handle, u16 len);

void __DLL ilitek_update_setting(void *handle,
				 struct ilitek_fw_settings *setting);

int __DLL ilitek_update_load_fw(void *handle, WCHAR *fw_name);

int __DLL ilitek_update_start(void *handle);

#ifdef __cplusplus
}
#endif

#endif
