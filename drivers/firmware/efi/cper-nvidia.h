/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UEFI Common Platform Error Record (CPER) support for NVIDIA sections
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef LINUX_CPER_NVIDIA_H
#define LINUX_CPER_NVIDIA_H

#include <linux/cper.h>

struct cper_sec_nvidia {
	char signature[16];
	__le16 error_type;
	__le16 error_instance;
	u8 severity;
	u8 socket;
	u8 number_regs;
	u8 reserved;
	__le64 instance_base;
} __packed;

#ifdef CONFIG_UEFI_CPER_NVIDIA
struct acpi_hest_generic_data;
void cper_estatus_print_nvidia(const char *pfx,
			       const struct acpi_hest_generic_data *gdata);
#else
static inline void cper_estatus_print_nvidia(const char *pfx,
					     const struct acpi_hest_generic_data *gdata) { }
#endif

#endif
