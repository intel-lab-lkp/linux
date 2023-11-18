// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2022-2023 Huawei Technologies Co., Ltd

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <errno.h>

#include "bpf-xfer-conf.h"
#include "spi-xfer-base.h"

#define CHIP_REGS_SIZE	0x8000

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CHIP_REGS_SIZE);
	__type(key, __u32);
	__type(value, __u32);
} regs_spi_xfer_r4v4 SEC(".maps");

static unsigned int chip_reg;

static int spi_xfer_read(struct spi_msg_ctx *msg, unsigned int len)
{
	unsigned int i, key = chip_reg;
	u32 *value;

	for (i = 0; i < len && i < sizeof(msg->data) - 3; i += 4, key++) {
		value = bpf_map_lookup_elem(&regs_spi_xfer_r4v4, &key);
		if (!value) {
			bpf_printk("key 0x%x not exists", key);
			return -EINVAL;
		}

		msg->data[i + 0] = (*value >> 24) & 0xff;
		msg->data[i + 1] = (*value >> 16) & 0xff;
		msg->data[i + 2] = (*value >> 8) & 0xff;
		msg->data[i + 3] = *value & 0xff;

		bpf_printk("SPI R32 [0x%x]=0x%x [%u/%u]", key, *value, i, len);
	}

	return 0;
}

static int spi_xfer_write(struct spi_msg_ctx *msg, unsigned int len)
{
	unsigned int i, key;
	u32 value;

	key = bpf_xfer_reg_u32((msg->data[0] << 24) | (msg->data[1] << 16) |
			       (msg->data[2] << 8) | msg->data[3]);
	chip_reg = key;

	for (i = 4; i < len && i < sizeof(msg->data) - 3; i += 4, key++) {
		value = msg->data[i];
		value = (value << 8) | msg->data[i + 1];
		value = (value << 8) | msg->data[i + 2];
		value = (value << 8) | msg->data[i + 3];

		if (bpf_map_update_elem(&regs_spi_xfer_r4v4, &key, &value, 0)) {
			bpf_printk("key 0x%x not exists", key);
			return -EINVAL;
		}

		bpf_printk("SPI W32 [0x%x]=0x%x [%u/%u]", key, value, i, len);
	}

	return 0;
}

SEC("raw_tp.w/spi_transfer_writeable")
int BPF_PROG(spi_xfer_r4v4, struct spi_msg_ctx *msg, u8 chip, unsigned int len)
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
