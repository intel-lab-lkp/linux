/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGTABLE_TYPES_H
#define _LINUX_PGTABLE_TYPES_H

#include <asm/page.h>

#ifndef __HAVE_ARCH_HW_PTE_T
#define hw_pte_t pte_t
#define __pte_from_hw(pte)	(pte)
#endif

#endif /* _LINUX_PGTABLE_TYPES_H */
