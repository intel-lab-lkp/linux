/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGTABLE_TYPES_H
#define _LINUX_PGTABLE_TYPES_H

#include <asm/page.h>

#ifndef __ASSEMBLY__

#ifdef CONFIG_ARCH_HAS_HW_PTE_T
typedef struct __hw_pte_t { pte_t __pte; } hw_pte_t;
#else
#define hw_pte_t pte_t
#endif

#endif /* !__ASSEMBLY__ */

#endif /* _LINUX_PGTABLE_TYPES_H */
