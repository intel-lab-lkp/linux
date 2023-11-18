// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2022-2023 Huawei Technologies Co., Ltd

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <errno.h>

#include "bpf-xfer-conf.h"
#include "spi-xfer-base.h"

#define CHIP_REGS_SIZE	0x100

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CHIP_REGS_SIZE);
	__type(key, __u32);
	__type(value, __u16);
} regs_spi_xfer_r1v2 SEC(".maps");

static unsigned int chip_reg;

static int spi_xfer_read(struct spi_msg_ctx *msg, unsigned int len)
{
	return spi_xfer_read_u16(msg, len, &regs_spi_xfer_r1v2, chip_reg, 0);
}

static int spi_xfer_write(struct spi_msg_ctx *msg, unsigned int len)
{
	chip_reg = bpf_xfer_reg_u8(msg->data[0]);
	return spi_xfer_write_u16(msg, len, &regs_spi_xfer_r1v2, chip_reg, 1);
}

SEC("raw_tp.w/spi_transfer_writeable")
int BPF_PROG(spi_xfer_r1v2, struct spi_msg_ctx *msg, u8 chip, unsigned int len)
{
	if (bpf_xfer_should_fault()) {
		msg->ret = -EIO;
		return 0;
	}

	if (msg->tx_nbits)
		msg->ret = spi_xfer_write(msg, len);
	else if (msg->rx_nbits)
		msg->ret = spi_xfer_read(msg, len);

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
