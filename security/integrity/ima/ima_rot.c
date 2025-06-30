// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Huawei Technologies Co., Ltd.
 *
 * Authors:
 * GONG Ruiqi <gongruiqi1@huawei.com>
 *
 * File: ima_rot.c
 *	IMA rot layer
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/gfp_types.h>

#include "ima.h"
#include "ima_tpm.h"

/*
 * Meaning of preferred_rot_name's value:
 *
 * - NULL: User doesn't specify one.
 * - NAME_UNKNOWN: User specifies an unknown RoT name.
 * - (Valid RoT name): User specifies a valid name.
 *
 * Which corresponds to cases of `ima_rot=`.
 */
static const char *preferred_rot_name;
#define NAME_UNKNOWN "UNKNOWN"

/*
 * The list containing all possible RoT devices.
 *
 * The order of RoTs inside the list implies priority.
 * IOW, RoT device that owns higher priority should be placed at the front.
 */
static struct ima_rot ima_rots[] = {
#ifdef CONFIG_TCG_TPM
	{
		.name = "tpm",
		.default_pcr = CONFIG_IMA_ROT_TPM_PCR_IDX,
		.init = ima_tpm_init,
		.extend = ima_tpm_extend,
		.calc_boot_aggregate = ima_tpm_calc_boot_aggregate,
	},
#endif
};

/**
 * ima_rot_name() - Process RoT device name that user provides.
 * @str: Name of RoT device.
 *
 * The back-end handler of `ima_rot=` kernel cmdline, by which users can specify
 * the Root of Trust (RoT) device for IMA. How system will react to each case of
 * this cmdline's value is explained as follows:
 *
 * - Absent: Choose an RoT device based on priority, try to initialize it and
 *   give up if it fails.
 *
 * - Invalid/Unknown RoT name: Give up RoT initialization. IMA will run without
 *   an RoT device.
 *
 * - Valid RoT name: Try to initialize this RoT device and give up if it fails.
 *
 * Return: Always return 1.
 */
static int __init ima_rot_name(char *str)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ima_rots); i++) {
		if (!strcmp(str, ima_rots[i].name)) {
			preferred_rot_name = str;
			break;
		}
	}

	if (!preferred_rot_name) {
		pr_info("%s is NOT implemented as an IMA RoT\n", str);
		preferred_rot_name = NAME_UNKNOWN;
	}

	return 1;
}
__setup("ima_rot=", ima_rot_name);

/*
 * Pick the most prioritized RoT that can be initialized successfully.
 */
struct ima_rot * __init ima_rot_init(void)
{
	int rc, i;

	for (i = 0; i < ARRAY_SIZE(ima_rots); i++) {
		if (preferred_rot_name && strcmp(preferred_rot_name, ima_rots[i].name))
			continue;

		pr_info("IMA RoT initializing %s\n", ima_rots[i].name);
		rc = ima_rots[i].init(&ima_rots[i]);
		if (!rc) {
			pr_info("%s initialized and taken as IMA RoT\n", ima_rots[i].name);
			return &ima_rots[i];
		}
		pr_debug("%s failed to self-initialize (%d)\n", ima_rots[i].name, rc);
	}

	return NULL;
}
