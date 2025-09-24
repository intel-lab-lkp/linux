// SPDX-License-Identifier: GPL-2.0
#include <linux/asi.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/string.h>

#include <asm/cmdline.h>
#include <asm/cpufeature.h>

#include "mm_internal.h"

/*
 * This is a bit like init_mm.pgd, it holds mappings shared among all ASI
 * domains.
 */
pgd_t *asi_nonsensitive_pgd;

void __init asi_check_boottime_disable(void)
{
	bool enabled = false;
	char arg[4];
	int ret;

	ret = cmdline_find_option(boot_command_line, "asi", arg, sizeof(arg));
	if (ret == 3 && !strncmp(arg, "off", 3)) {
		enabled = false;
		pr_info("ASI explicitly disabled by kernel cmdline.\n");
	} else if (ret == 2 && !strncmp(arg, "on", 2)) {
		enabled = true;
		pr_info("ASI enabled.\n");
	} else if (ret) {
		pr_err("Unknown asi= flag '%s', try 'off' or 'on'\n", arg);
	}

	if (enabled)
		setup_force_cpu_cap(X86_FEATURE_ASI);
}

void __init asi_init(void)
{
	if (!cpu_feature_enabled(X86_FEATURE_ASI))
		return;

	asi_nonsensitive_pgd = alloc_low_page();
	if (WARN_ON(!asi_nonsensitive_pgd))
		setup_clear_cpu_cap(X86_FEATURE_ASI);
}
