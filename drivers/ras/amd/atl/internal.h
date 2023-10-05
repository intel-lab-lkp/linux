/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD Address Translation Library
 *
 * internal.h : Helper functions and common defines
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#ifndef __AMD_ATL_INTERNAL_H__
#define __AMD_ATL_INTERNAL_H__

#include <asm/amd_nb.h>

#include <linux/amd-atl.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>

#include "reg_fields.h"

/* Maximum possible number of Coherent Stations within a single Data Fabric. */
#define MAX_CS_CHANNELS			32

/* PCI IDs for Genoa DF Function 0. */
#define DF_FUNC0_ID_GENOA		0x14AD1022

/* Shift needed for adjusting register values to true values. */
#define DF_DRAM_BASE_LIMIT_LSB		28

/*
 * Glossary of acronyms used in address translation for Zen-based systems
 *
 * CCM		= Cache Coherent Moderator
 * COD		= Cluster-on-Die
 * CS		= Coherent Station
 * DF		= Data Fabric
 */

enum df_revisions {
	UNKNOWN,
	DF2,
	DF3,
	DF3p5,
	DF4,
	DF4p5,
};

/* These are mapped 1:1 to the hardware values. Special cases are set at > 0x20. */
enum intlv_modes {
	NONE				= 0x00,
	NOHASH_2CHAN			= 0x01,
	NOHASH_4CHAN			= 0x03,
	NOHASH_8CHAN			= 0x05,
	DF3_6CHAN			= 0x06,
	NOHASH_16CHAN			= 0x07,
	NOHASH_32CHAN			= 0x08,
	DF3_COD4_2CHAN_HASH		= 0x0C,
	DF3_COD2_4CHAN_HASH		= 0x0D,
	DF3_COD1_8CHAN_HASH		= 0x0E,
	DF4_NPS4_2CHAN_HASH		= 0x10,
	DF4_NPS2_4CHAN_HASH		= 0x11,
	DF4_NPS1_8CHAN_HASH		= 0x12,
	DF4_NPS4_3CHAN_HASH		= 0x13,
	DF4_NPS2_6CHAN_HASH		= 0x14,
	DF4_NPS1_12CHAN_HASH		= 0x15,
	DF4_NPS2_5CHAN_HASH		= 0x16,
	DF4_NPS1_10CHAN_HASH		= 0x17,
	DF2_2CHAN_HASH			= 0x21,
	/* DF4.5 modes are all IntLvNumChan + 0x20 */
	DF4p5_NPS1_16CHAN_1K_HASH	= 0x2C,
	DF4p5_NPS0_24CHAN_1K_HASH	= 0x2E,
	DF4p5_NPS4_2CHAN_1K_HASH	= 0x30,
	DF4p5_NPS2_4CHAN_1K_HASH	= 0x31,
	DF4p5_NPS1_8CHAN_1K_HASH	= 0x32,
	DF4p5_NPS4_3CHAN_1K_HASH	= 0x33,
	DF4p5_NPS2_6CHAN_1K_HASH	= 0x34,
	DF4p5_NPS1_12CHAN_1K_HASH	= 0x35,
	DF4p5_NPS2_5CHAN_1K_HASH	= 0x36,
	DF4p5_NPS1_10CHAN_1K_HASH	= 0x37,
	DF4p5_NPS4_2CHAN_2K_HASH	= 0x40,
	DF4p5_NPS2_4CHAN_2K_HASH	= 0x41,
	DF4p5_NPS1_8CHAN_2K_HASH	= 0x42,
	DF4p5_NPS1_16CHAN_2K_HASH	= 0x43,
	DF4p5_NPS4_3CHAN_2K_HASH	= 0x44,
	DF4p5_NPS2_6CHAN_2K_HASH	= 0x45,
	DF4p5_NPS1_12CHAN_2K_HASH	= 0x46,
	DF4p5_NPS0_24CHAN_2K_HASH	= 0x47,
	DF4p5_NPS2_5CHAN_2K_HASH	= 0x48,
	DF4p5_NPS1_10CHAN_2K_HASH	= 0x49,
};

struct df_flags {
	__u8	legacy_ficaa		: 1,
		genoa_quirk		: 1,
		__reserved_0		: 6;
};

struct df_config {
	enum df_revisions rev;

	/*
	 * These masks operate on the 16-bit CS IDs,
	 * e.g. Instance, Fabric, Destination, etc.
	 */
	u16 component_id_mask;
	u16 die_id_mask;
	u16 node_id_mask;
	u16 socket_id_mask;

	/*
	 * Least-significant bit of Node ID portion of the
	 * system-wide CS Fabric ID.
	 */
	u8 node_id_shift;

	/*
	 * Least-significant bit of Die portion of the Node ID.
	 * Adjusted to include the Node ID shift in order to apply
	 * to the CS Fabric ID.
	 */
	u8 die_id_shift;

	/*
	 * Least-significant bit of Socket portion of the Node ID.
	 * Adjusted to include the Node ID shift in order to apply
	 * to the CS Fabric ID.
	 */
	u8 socket_id_shift;

	/* Number of DRAM Address maps visible in a CS. */
	u8 num_cs_maps;

	/* Global flags to handle special cases. */
	struct df_flags flags;
};

extern struct df_config df_cfg;

struct dram_addr_map {
	/*
	 * Each DRAM Address Map can operate independently
	 * in different interleaving modes.
	 */
	enum intlv_modes intlv_mode;

	/* System-wide number for this address map. */
	u8 num;

	/* Raw register values */
	u32 base;
	u32 limit;
	u32 ctl;
	u32 intlv;

	/*
	 * Logical to Physical CS Remapping array
	 *
	 * Index: Logical CS Instance ID
	 * Value: Physical CS Instance ID
	 *
	 * phys_cs_inst_id = remap_array[log_cs_inst_id]
	 */
	u8 remap_array[MAX_CS_CHANNELS];

	/*
	 * Number of bits covering DRAM Address map 0
	 * when interleaving is non-power-of-2.
	 *
	 * Used only for DF3_6CHAN.
	 */
	u8 np2_bits;

	/* Position of the 'interleave bit'. */
	u8 intlv_bit_pos;
	/* Number of channels interleaved in this map. */
	u8 num_intlv_chan;
	/* Number of dies interleaved in this map. */
	u8 num_intlv_dies;
	/* Number of sockets interleaved in this map. */
	u8 num_intlv_sockets;
	/*
	 * Total number of channels interleaved accounting
	 * for die and socket interleaving.
	 */
	u8 total_intlv_chan;
	/* Total bits needed to cover 'total_intlv_chan'. */
	u8 total_intlv_bits;
};

struct addr_ctx {
	u64 ret_addr;

	struct dram_addr_map map;

	/* AMD Node ID calculated from Socket and Die IDs. */
	u8 node_id;

	/*
	 * CS Instance ID
	 * Local ID used within a 'node'.
	 */
	u16 inst_id;

	/*
	 * CS Fabric ID
	 * System-wide ID that includes 'node' bits.
	 */
	u16 cs_fabric_id;
};

int df_indirect_read_instance(u16 node, u8 func, u16 reg, u8 instance_id, u32 *lo);
int df_indirect_read_broadcast(u16 node, u8 func, u16 reg, u32 *lo);

int get_df_system_info(void);
int determine_node_id(struct addr_ctx *ctx, u8 socket_num, u8 die_num);

int get_address_map(struct addr_ctx *ctx);

int denormalize_address(struct addr_ctx *ctx);
int dehash_address(struct addr_ctx *ctx);

int norm_to_sys_addr(u8 socket_id, u8 die_id, u8 cs_inst_id, u64 *addr);

/*
 * Helper to use test_bit() without needing to do
 * a cast to "unsigned long *" everywhere.
 *
 * Use this for dynamic checks, i.e. when FIELD_GET()
 * won't work.
 */
static inline bool atl_get_bit(u8 bit_num, u64 data)
{
	return test_bit(bit_num, (unsigned long *)&data);
}

/*
 * Make a gap in 'data' that is 'num_bits' long starting at 'bit_num.
 * e.g. data		= 11111111'b
 *	bit_num		= 3
 *	num_bits	= 2
 *	result		= 1111100111'b
 */
static inline u64 expand_bits(u8 bit_num, u8 num_bits, u64 data)
{
	u64 temp1, temp2;

	/*
	 * Return the orginal data if the "space" needed is '0'.
	 * This helps avoid the need to check for '0' at each
	 * caller.
	 */
	if (!num_bits)
		return data;

	if (!bit_num)
		return data << num_bits;

	temp1 = data & GENMASK_ULL(bit_num - 1, 0);

	temp2 = data & GENMASK_ULL(63, bit_num);
	temp2 <<= num_bits;

	return temp1 | temp2;
}

/*
 * Remove bits in 'data' between low_bit and high_bit inclusive.
 * e.g. data		= XXXYYZZZ'b
 *	low_bit		= 3
 *	high_bit	= 4
 *	result		= XXXZZZ'b
 */
static inline u64 remove_bits(u8 low_bit, u8 high_bit, u64 data)
{
	u64 temp1, temp2;

	if (high_bit >= 64 || low_bit >= 64 || low_bit > high_bit) {
		pr_warn("%s: Invalid inputs: low_bit=%u high_bit=%u",
			__func__, low_bit, high_bit);
		return 0;
	}

	if (!low_bit)
		return data >> (high_bit++);

	temp1 = GENMASK_ULL(low_bit - 1, 0) & data;
	temp2 = GENMASK_ULL(63, high_bit + 1) & data;
	temp2 >>= high_bit - low_bit + 1;

	return temp1 | temp2;
}

#define ATL_BAD_DF_REV \
	({ pr_warn("%s: Unrecognized DF rev: %u", \
		   __func__, df_cfg.rev); })

#define ATL_BAD_INTLV_MODE(mode) \
	({pr_warn("%s: Unrecognized interleave mode: %u", \
		  __func__, mode); })

#define atl_debug(fmt, arg...) \
	pr_debug("AMD ATL: %s: " fmt, __func__, ##arg)

#endif /* __AMD_ATL_INTERNAL_H__ */
