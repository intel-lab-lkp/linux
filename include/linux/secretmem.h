/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _LINUX_SECRETMEM_H
#define _LINUX_SECRETMEM_H

#ifdef CONFIG_SECRETMEM

bool secretmem_active(void);

#else

static inline bool secretmem_active(void)
{
	return false;
}

#endif /* CONFIG_SECRETMEM */

#endif /* _LINUX_SECRETMEM_H */
