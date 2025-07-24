/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AMD Platform Security Processor (PSP) Seamless Firmware (SFS) Support.
 *
 * Copyright (C) 2023-2025 Advanced Micro Devices, Inc.
 *
 * Author: Ashish Kalra <ashish.kalra@amd.com>
 */

#ifndef __SFS_H__
#define __SFS_H__

#include <uapi/linux/psp-sfs.h>

#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/psp-sev.h>
#include <linux/psp-platform-access.h>
#include <linux/set_memory.h>

#include <asm/sev.h>

#include "psp-dev.h"

struct psp_sfs_device {
	struct device *dev;
	struct psp_device *psp;

	struct sfs_command *command_hdr;

	struct mutex ioctl_mutex;

	struct miscdevice char_dev;

	struct page *page;

	/* used to abstract communication path */
	u32	header_size;
	u32	*payload_size;
	u32	*result;
	void	*payload;
	void	*pkg_hdr;
};

struct sfs_command {
	struct psp_ext_request ext_req;
};

void sfs_dev_destroy(struct psp_device *psp);
int sfs_dev_init(struct psp_device *psp);
void sfs_pci_init(void);

#endif /* __SFS_H__ */
