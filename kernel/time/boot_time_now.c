// SPDX-License-Identifier: LGPL-2.0

#include <linux/boot_time_now.h>
#include <asm/boot_time_primitives.h>

u64 boot_time_now(void)
{
	return arch_boot_counter_now();
}
EXPORT_SYMBOL_GPL(boot_time_now);

MODULE_DESCRIPTION("boot time tracker");
MODULE_LICENSE("GPL");
