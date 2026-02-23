/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UEFI Common Platform Error Record (CPER) support for NVIDIA sections
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/cper.h>
#include <linux/unaligned.h>
#include <acpi/ghes.h>
#include "cper-nvidia.h"

static void cper_print_nvidia_error(const char *pfx,
				    const struct cper_sec_nvidia *nvidia_err,
				    size_t error_data_length)
{
	int i;
	const u8 *reg_data;
	size_t min_size;

	printk("%s""signature: %.16s\n", pfx, nvidia_err->signature);
	printk("%s""error_type: %u\n", pfx, le16_to_cpu(nvidia_err->error_type));
	printk("%s""error_instance: %u\n", pfx, le16_to_cpu(nvidia_err->error_instance));
	printk("%s""severity: %u\n", pfx, nvidia_err->severity);
	printk("%s""socket: %u\n", pfx, nvidia_err->socket);
	printk("%s""number_regs: %u\n", pfx, nvidia_err->number_regs);
	printk("%s""instance_base: 0x%016llx\n", pfx,
	       (unsigned long long)le64_to_cpu(nvidia_err->instance_base));

	if (nvidia_err->number_regs == 0)
		return;

	/*
	 * Validate that all registers fit within the error_data_length.
	 * Each register pair is 16 bytes (two u64s).
	 */
	min_size = sizeof(*nvidia_err) + (nvidia_err->number_regs * 16);
	if (error_data_length < min_size) {
		printk("%s""NVIDIA: Invalid number_regs %u (section size %zu, need %zu)\n",
		       pfx, nvidia_err->number_regs, error_data_length, min_size);
		return;
	}

	/*
	 * Registers are stored as address-value pairs immediately
	 * following the fixed header. Each pair is two little-endian u64s.
	 */
	reg_data = (const u8 *)(nvidia_err + 1);
	for (i = 0; i < nvidia_err->number_regs; i++) {
		u64 addr = get_unaligned_le64(reg_data + i * 16);
		u64 val = get_unaligned_le64(reg_data + i * 16 + 8);

		printk("%s""register[%d]: address=0x%016llx value=0x%016llx\n",
		       pfx, i, (unsigned long long)addr, (unsigned long long)val);
	}
}

void cper_estatus_print_nvidia(const char *pfx,
			       const struct acpi_hest_generic_data *gdata)
{
	struct cper_sec_nvidia *nvidia_err;

	nvidia_err = acpi_hest_get_payload((struct acpi_hest_generic_data *)gdata);
	if (!nvidia_err) {
		printk("%s""NVIDIA error: Failed to get payload\n", pfx);
		return;
	}

	printk("%s""section_type: NVIDIA, error_data_length: %u\n", pfx, gdata->error_data_length);

	if (gdata->error_data_length < sizeof(*nvidia_err)) {
		printk("%s""NVIDIA error: Section too small (%u < %zu)\n",
		       pfx, gdata->error_data_length, sizeof(*nvidia_err));
		return;
	}

	cper_print_nvidia_error(pfx, nvidia_err, gdata->error_data_length);
}
