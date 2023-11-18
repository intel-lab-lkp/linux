/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The spi xfer helpers for bpf program
 *
 * Copyright (C) 2022-2023 Huawei Technologies Co., Ltd
 */

#ifndef __SPI_XFER_BASE_
#define __SPI_XFER_BASE_

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

int spi_xfer_read_u8(struct spi_msg_ctx *msg, unsigned int len,
		     void *map, unsigned int reg, int offset)
{
	unsigned int i, key = reg;
	u8 *value;

	for (i = offset; i < len && i < sizeof(msg->data); i++, key++) {
		value = bpf_map_lookup_elem(map, &key);
		if (!value) {
			bpf_printk("key 0x%x not exists", key);
			return -1;
		}

		msg->data[i] = *value;

		bpf_printk("SPI R8 [0x%x]=0x%x", key, msg->data[i]);
	}

	return 0;
}

int spi_xfer_write_u8(struct spi_msg_ctx *msg, unsigned int len,
		      void *map, unsigned int reg, int offset)
{
	unsigned int i, key = reg;
	u8 value;

	for (i = offset; i < len && i < sizeof(msg->data); i++, key++) {
		value = msg->data[i];

		if (bpf_map_update_elem(map, &key, &value, BPF_EXIST)) {
			bpf_printk("key 0x%x not exists", key);
			return -1;
		}

		bpf_printk("SPI W8 [0x%x]=0x%x [%u/%u]", key, value, i, len);
	}

	return 0;
}

int spi_xfer_read_u16(struct spi_msg_ctx *msg, unsigned int len,
		      void *map, unsigned int reg, int offset)
{
	unsigned int i, key = reg;
	u16 *value;

	for (i = offset; i < len && i < sizeof(msg->data) - 1; i += 2, key++) {
		value = bpf_map_lookup_elem(map, &key);
		if (!value) {
			bpf_printk("key 0x%x not exists", key);
			return -1;
		}

		msg->data[i + 0] = *value >> 8;
		msg->data[i + 1] = *value & 0xff;

		bpf_printk("SPI R16 [0x%x]=0x%x [%u/%u]", key, *value, i, len);
	}

	return 0;
}

int spi_xfer_write_u16(struct spi_msg_ctx *msg, unsigned int len,
		       void *map, unsigned int reg, int offset)
{
	unsigned int i, key = reg;
	u16 value;

	for (i = offset; i < len && i < sizeof(msg->data) - 1; i += 2, key++) {
		value = msg->data[i];
		value = (value << 8) | msg->data[i + 1];

		if (bpf_map_update_elem(map, &key, &value, BPF_EXIST)) {
			bpf_printk("key 0x%x not exists", key);
			return -1;
		}

		bpf_printk("SPI W16 [0x%x]=0x%x [%u/%u]", key, value, i, len);
	}

	return 0;
}

#endif
