/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 - 2025 Intel Corporation
 */

#ifndef IPU7_FW_COMMOM_ABI_H
#define IPU7_FW_COMMOM_ABI_H

#include <linux/types.h>

#pragma pack(push, 1)
typedef u32	ia_gofo_addr_t;

struct ia_gofo_version_s {
	u8 patch;
	u8 subminor;
	u8 minor;
	u8 major;
};

#define IA_GOFO_MSG_VERSION_LIST_MAX_ENTRIES	(3U)
#define IA_GOFO_MSG_RESERVED_SIZE		(3U)

struct ia_gofo_msg_version_list {
	u8 num_versions;
	u8 reserved[IA_GOFO_MSG_RESERVED_SIZE];
	struct ia_gofo_version_s versions[IA_GOFO_MSG_VERSION_LIST_MAX_ENTRIES];
};

#pragma pack(pop)

#define IA_GOFO_MSG_ERR_MAX_DETAILS		(4U)
#define IA_GOFO_MSG_ERR_OK			(0U)
#define IA_GOFO_MSG_ERR_GROUP_UNSPECIFIED	(0U)
#define IA_GOFO_MSG_ERR_IS_OK(err)	(IA_GOFO_MSG_ERR_OK == (err).err_code)

#pragma pack(push, 1)
struct ia_gofo_msg_err {
	u32 err_group;
	u32 err_code;
	u32 err_detail[IA_GOFO_MSG_ERR_MAX_DETAILS];
};

#pragma pack(pop)

#define IA_GOFO_MSG_ERR_GROUP_RESERVED	IA_GOFO_MSG_ERR_GROUP_UNSPECIFIED
#define IA_GOFO_MSG_ERR_GROUP_GENERAL		1

#define IA_GOFO_MSG_TYPE_RESERVED	0
#define IA_GOFO_MSG_TYPE_INDIRECT	1
#define IA_GOFO_MSG_TYPE_LOG		2
#define IA_GOFO_MSG_TYPE_GENERAL_ERR	3

#pragma pack(push, 1)
enum ia_gofo_msg_link_streaming_mode {
	IA_GOFO_MSG_LINK_STREAMING_MODE_SOFF = 0,
};

#pragma pack(pop)

#pragma pack(push, 1)
#define IA_GOFO_MSG_LOG_MAX_PARAMS	(4U)

struct ia_gofo_msg_log_info {
	u16 log_counter;
	u8 msg_parameter_types;
	/* [0:0] is_out_of_order, [1:3] logger_channel, [4:7] reserved */
	u8 logger_opts;
	u32 fmt_id;
	u32 params[IA_GOFO_MSG_LOG_MAX_PARAMS];
};

struct ia_gofo_msg_log_info_ts {
	u64 msg_ts;
	struct ia_gofo_msg_log_info log_info;
};

#pragma pack(pop)

#define IA_GOFO_MSG_ABI_OUT_LOG_QUEUE_ID	(1U)

#endif
