/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_USER_PROCESS_H
#define _LINUX_USER_PROCESS_H
#include <linux/mm.h>

/*
 * mseal of userspace process's system mappings.
 */
static inline unsigned long mseal_system_mappings(void)
{
#ifdef CONFIG_MSEAL_SYSTEM_MAPPINGS
	return VM_SEALED;
#else
	return 0;
#endif
}

#endif
