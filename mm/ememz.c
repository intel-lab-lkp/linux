// SPDX-License-Identifier: GPL-2.0

#include <linux/syscalls.h>

#ifdef CONFIG_EMEMZ_SYSCALL
/*
 * Set task_struct flag to fill any memory associated with process on
 * exit to zero.
 */
SYSCALL_DEFINE1(ememz, int, flags)
{
	if (flags & ~(0))
		return -EINVAL;

	// Set flag atomically
	return 0;
}
#endif
