/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bpf program of control xfer configuration
 *
 * Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
 */

#ifndef __BPF_XFER_CONF_
#define __BPF_XFER_CONF_

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

#define CONF_REGS_SIZE		0x100

#define BPF_CONF_REG_MASK	0x10
#define BPF_CONF_REG_RSHIFT	0x11
#define BPF_CONF_REG_LSHIFT	0x12
#define BPF_CONF_REG_XBSWAP	0x13

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CONF_REGS_SIZE);
	__type(key, __u32);
	__type(value, __u32);
} bpf_xfer_conf SEC(".maps");

static u32 bpf_reg_mask, bpf_reg_xbswap;
static u32 bpf_reg_rshift, bpf_reg_lshift;

static u32 bpf_xfer_read_conf(u32 key)
{
	u32 *reg;

	reg = bpf_map_lookup_elem(&bpf_xfer_conf, &key);
	if (!reg) {
		bpf_printk("config key %d not exists", key);
		return 0;
	}

	return *reg;
}

static int bpf_xfer_write_conf(u32 key, u32 value)
{
	if (bpf_map_update_elem(&bpf_xfer_conf, &key, &value,
				BPF_EXIST)) {
		bpf_printk("config key %d not exists", key);
		return -1;
	}

	return 0;
}

static int bpf_xfer_update_config(void)
{
	bpf_reg_mask = bpf_xfer_read_conf(BPF_CONF_REG_MASK);
	bpf_reg_rshift = bpf_xfer_read_conf(BPF_CONF_REG_RSHIFT);
	bpf_reg_lshift = bpf_xfer_read_conf(BPF_CONF_REG_LSHIFT);
	bpf_reg_xbswap = bpf_xfer_read_conf(BPF_CONF_REG_XBSWAP);

	return 0;
}

u8 bpf_xfer_reg_u8(u8 reg)
{
	reg = reg & (bpf_reg_mask ? bpf_reg_mask : 0xff);
	if (bpf_reg_rshift)
		reg = reg >> bpf_reg_rshift;
	if (bpf_reg_lshift)
		reg = reg << bpf_reg_lshift;
	return reg;
}

u16 bpf_xfer_reg_u16(u16 reg)
{
	if (bpf_reg_xbswap)
		reg = __builtin_bswap16(reg);
	reg = reg & (bpf_reg_mask ? bpf_reg_mask : 0x7fff);
	if (bpf_reg_rshift)
		reg = reg >> bpf_reg_rshift;
	if (bpf_reg_lshift)
		reg = reg << bpf_reg_lshift;
	return reg;
}

u32 bpf_xfer_reg_u24(u32 reg)
{
	if (bpf_reg_xbswap)
		reg = __builtin_bswap32(reg);
	reg = reg & (bpf_reg_mask ? bpf_reg_mask : 0x7fffff);
	if (bpf_reg_rshift)
		reg = reg >> bpf_reg_rshift;
	if (bpf_reg_lshift)
		reg = reg << bpf_reg_lshift;
	return reg;
}

u32 bpf_xfer_reg_u32(u32 reg)
{
	if (bpf_reg_xbswap)
		reg = __builtin_bswap32(reg);
	reg = reg & (bpf_reg_mask ? bpf_reg_mask : 0x7fffffff);
	if (bpf_reg_rshift)
		reg = reg >> bpf_reg_rshift;
	if (bpf_reg_lshift)
		reg = reg << bpf_reg_lshift;
	return reg;
}
#endif
