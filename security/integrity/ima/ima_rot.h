/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Huawei Technologies Co., Ltd.
 *
 * Authors:
 * GONG Ruiqi <gongruiqi1@huawei.com>
 *
 * File: ima_rot.h
 *	IMA rot layer
 */

#ifndef __LINUX_IMA_ROT_H
#define __LINUX_IMA_ROT_H

#include <linux/tpm.h>

/**
 * struct ima_rot - Root of Trust (RoT) device instance.
 * @name: Name of the device.
 * @default_pcr: Index of default (virtual) PCR.
 * @nr_allocated_banks: Number of (virtual) TPM banks.
 * @allocated_banks: Info of (virtual) TPM bank.
 * @init: Initializes this device to be an IMA RoT.
 * @extend: Performs a hash extend operation to the device's (virtual) PCR.
 * @calc_boot_aggregate: Generate the initial value of IMA measurement list
 *                       (i.e. boot aggregate).
 *
 * Attributes and methods necessary for a device working as an RoT for IMA.
 */
struct ima_rot {
	const char *name;
	int default_pcr;
	int nr_allocated_banks;
	struct tpm_bank_info *allocated_banks;

	int (*init)(struct ima_rot *rot);
	int (*extend)(struct tpm_digest *digests_arg, const void *args);
	int (*calc_boot_aggregate)(struct ima_digest_data *hash);
};

struct ima_rot *ima_rot_init(void);
#endif /* __LINUX_IMA_ROT_H */
