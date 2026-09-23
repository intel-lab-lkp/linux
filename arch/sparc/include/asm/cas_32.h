/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC_CAS_32_H
#define _SPARC_CAS_32_H

#include <linux/types.h>

/* Returns 0 and stores the value found in *prev, or -EFAULT. */
int __sparc32_casa(u32 *addr, u32 oldval, u32 newval, u32 *prev);

#endif /* _SPARC_CAS_32_H */
