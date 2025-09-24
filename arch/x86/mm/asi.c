// SPDX-License-Identifier: GPL-2.0
#include <linux/asi.h>
#include <linux/init.h>
#include <linux/string.h>

#include <asm/cmdline.h>
#include <asm/cpufeature.h>

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
