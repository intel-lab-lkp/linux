/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _INCLUDE_ASI_H
#define _INCLUDE_ASI_H

#ifdef CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION
#include <asm/asi.h>
#else

#include <linux/types.h>

static inline void asi_check_boottime_disable(void) { }
static inline bool asi_enabled_static(void) { return false; }

#define asi_nonsensitive_pgd NULL

static inline void asi_init(void) { };

#endif /* CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION */
#endif /* _INCLUDE_ASI_H */
