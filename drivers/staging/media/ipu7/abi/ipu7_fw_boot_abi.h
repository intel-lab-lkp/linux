/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 - 2025 Intel Corporation
 */

#ifndef IPU7_FW_BOOT_ABI_H
#define IPU7_FW_BOOT_ABI_H

#include "ipu7_fw_common_abi.h"
#include "ipu7_fw_syscom_abi.h"

#define IA_GOFO_FWLOG_MAX_LOGGER_SOURCES		(64U)

#define LOGGER_CONFIG_CHANNEL_ENABLE_SYSCOM_BITMASK	BIT(1)

struct ia_gofo_logger_config {
	u8 use_source_severity;
	u8 source_severity[IA_GOFO_FWLOG_MAX_LOGGER_SOURCES];
	u8 use_channels_enable_bitmask;
	u8 channels_enable_bitmask;
	u8 padding[1];
	ia_gofo_addr_t hw_printf_buffer_base_addr;
	u32 hw_printf_buffer_size_bytes;
};

#pragma pack(push, 1)

#define IA_GOFO_BOOT_RESERVED_SIZE (58U)

enum ia_gofo_buttress_reg_id {
	IA_GOFO_FW_BOOT_CONFIG_ID = 0,
	IA_GOFO_FW_BOOT_STATE_ID = 1,
	IA_GOFO_FW_BOOT_SYSCOM_QUEUE_INDICES_BASE_ID = 2,
	IA_GOFO_FW_BOOT_MESSAGING_VERSION_ID = 4,
	IA_GOFO_FW_BOOT_ID_MAX
};

enum ia_gofo_boot_uc_tile_frequency_units {
	IA_GOFO_FW_BOOT_UC_FREQUENCY_UNITS_MHZ = 0,
};

#define IA_GOFO_FW_BOOT_STATE_IS_CRITICAL(boot_state) \
	(0xdead0000 == ((boot_state) & 0xffff0000))

struct ia_gofo_boot_config {
	u32 length;
	struct ia_gofo_version_s config_version;
	struct ia_gofo_msg_version_list client_version_support;
	ia_gofo_addr_t pkg_dir;
	ia_gofo_addr_t subsys_config;
	u32 uc_tile_frequency;
	u16 checksum;
	u8 uc_tile_frequency_units;
	u8 padding[1];
	u32 reserved[IA_GOFO_BOOT_RESERVED_SIZE];
	struct syscom_config_s syscom_context_config;
};

#pragma pack(pop)

#define IA_GOFO_WDT_TIMEOUT_ERR			0xdead0401
#define IA_GOFO_MEM_FATAL_DME_ERR		0xdead0801
#define IA_GOFO_MEM_UNCORRECTABLE_LOCAL_ERR	0xdead0802
#define IA_GOFO_MEM_UNCORRECTABLE_DIRTY_ERR	0xdead0803
#define IA_GOFO_MEM_UNCORRECTABLE_DTAG_ERR	0xdead0804
#define IA_GOFO_MEM_UNCORRECTABLE_CACHE_ERR	0xdead0805
#define IA_GOFO_DOUBLE_EXCEPTION_ERR		0xdead0806
#define IA_GOFO_BIST_DMEM_FAULT_DETECTION_ERR	0xdead1000

enum ia_gofo_boot_state {
	IA_GOFO_FW_BOOT_STATE_UNINIT = 0x57a7e000,
	IA_GOFO_FW_BOOT_STATE_READY = 0x57a7e100,
	IA_GOFO_FW_BOOT_STATE_SHUTDOWN_CMD = 0x57a7f001,
	IA_GOFO_FW_BOOT_STATE_INACTIVE = 0x57a7e300,
};

#endif
