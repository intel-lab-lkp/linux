// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SVM-SEV command line parsing support
 *
 * Copyright (C) 2023 - 2024 Advanced Micro Devices, Inc.
 *
 * Author: Michael Roth <michael.roth@amd.com>
 *
 */

#include <linux/memblock.h>

#include <asm/sev.h>

struct sev_config sev_cfg;

static int __init init_sev_config(char *str)
{
	char *s;

	while ((s = strsep(&str, ","))) {
		if (!strcmp(s, "debug")) {
			sev_cfg.debug = true;
			continue;
		}

		if (!strcmp(s, "nosnp")) {
			setup_clear_cpu_cap(X86_FEATURE_SEV_SNP);
			cc_platform_clear(CC_ATTR_HOST_SEV_SNP);
			continue;
		}

		pr_info("SEV command-line option '%s' was not recognized\n", s);
	}

	return 1;
}
__setup("sev=", init_sev_config);
