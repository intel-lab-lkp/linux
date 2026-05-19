// SPDX-License-Identifier: GPL-2.0
#include "linux/export.h"
#include <linux/sched.h>
#include <linux/pwlocks.h>
#include <linux/string.h>
#include <linux/sched/isolation.h>

DEFINE_STATIC_KEY_MAYBE(CONFIG_PWLOCKS_DEFAULT, pw_sl);
EXPORT_SYMBOL(pw_sl);

static bool pwlocks_param_specified;

static int __init pwlocks_setup(char *str)
{
	int opt;

	if (!get_option(&str, &opt)) {
		pr_warn("PWLOCKS: invalid pwlocks parameter: %s, ignoring.\n", str);
		return 0;
	}

	if (opt)
		static_branch_enable(&pw_sl);
	else
		static_branch_disable(&pw_sl);

	pwlocks_param_specified = true;

	return 1;
}
__setup("pwlocks=", pwlocks_setup);

/*
 * Enable PWLOCKS if CPUs want to avoid kernel noise.
 */
static int __init pwlocks_init(void)
{
	if (pwlocks_param_specified)
		return 0;

	if (housekeeping_enabled(HK_TYPE_KERNEL_NOISE))
		static_branch_enable(&pw_sl);

	return 0;
}

late_initcall(pwlocks_init);
