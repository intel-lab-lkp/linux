// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef VIRTIO_VFIO_CMD_H
#define VIRTIO_VFIO_CMD_H

#include <linux/kernel.h>
#include <linux/virtio.h>
#include <linux/vfio_pci_core.h>
#include <linux/virtio_pci.h>

struct virtiovf_pci_core_device {
	struct vfio_pci_core_device core_device;
	u8 bar0_virtual_buf_size;
	u8 *bar0_virtual_buf;
	/* synchronize access to the virtual buf */
	struct mutex bar_mutex;
	int vf_id;
	void __iomem *notify_addr;
	u32 notify_offset;
	u8 notify_bar;
	u8 pci_cmd_io :1;
};

int virtiovf_cmd_list_query(struct pci_dev *pdev, u8 *buf, int buf_size);
int virtiovf_cmd_list_use(struct pci_dev *pdev, u8 *buf, int buf_size);
int virtiovf_cmd_lr_write(struct virtiovf_pci_core_device *virtvdev, u16 opcode,
			  u8 offset, u8 size, u8 *buf);
int virtiovf_cmd_lr_read(struct virtiovf_pci_core_device *virtvdev, u16 opcode,
			 u8 offset, u8 size, u8 *buf);
int virtiovf_cmd_lq_read_notify(struct virtiovf_pci_core_device *virtvdev,
				u8 req_bar_flags, u8 *bar, u64 *bar_offset);
#endif /* VIRTIO_VFIO_CMD_H */
