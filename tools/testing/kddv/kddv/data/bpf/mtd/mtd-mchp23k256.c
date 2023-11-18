// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2022-2023 Huawei Technologies Co., Ltd */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <errno.h>

#include "spi-xfer-base.h"

#define MCHP23K256_CMD_WRITE_STATUS	0x01
#define MCHP23K256_CMD_WRITE		0x02
#define MCHP23K256_CMD_READ		0x03

#define CHIP_REGS_SIZE			0x20000

#define MAX_CMD_SIZE			4

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CHIP_REGS_SIZE);
	__type(key, __u32);
	__type(value, __u8);
} regs_mtd_mchp23k256 SEC(".maps");

static unsigned int chip_reg;

static int spi_transfer_read(struct spi_msg_ctx *msg, unsigned int len)
{
	return spi_xfer_read_u8(msg, len, &regs_mtd_mchp23k256, chip_reg, 0);
}

static int spi_transfer_write(struct spi_msg_ctx *msg, unsigned int len)
{
	u8 opcode = msg->data[0];
	int i;

	switch (opcode) {
	case MCHP23K256_CMD_READ:
	case MCHP23K256_CMD_WRITE:
		if (len < 2)
			return -EINVAL;

		chip_reg = 0;
		for (i = 0; i < MAX_CMD_SIZE && i < len - 1; i++)
			chip_reg = (chip_reg << 8) + msg->data[1 + i];

		return 0;
	case MCHP23K256_CMD_WRITE_STATUS:
		// ignore write status
		return 0;
	default:
		break;
	}

	return spi_xfer_write_u8(msg, len, &regs_mtd_mchp23k256, chip_reg, 0);
}

SEC("raw_tp.w/spi_transfer_writeable")
int BPF_PROG(mtd_mchp23k256, struct spi_msg_ctx *msg, u8 chip, unsigned int len)
{
	int ret = 0;

	if (msg->tx_nbits)
		ret = spi_transfer_write(msg, len);
	else if (msg->rx_nbits)
		ret = spi_transfer_read(msg, len);

	return ret;
}

char LICENSE[] SEC("license") = "GPL";
