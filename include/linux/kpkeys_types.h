/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KPKEYS_TYPES_H
#define _LINUX_KPKEYS_TYPES_H

#include <linux/types.h>

#ifdef CONFIG_ARCH_HAS_KPKEYS
#include <asm/kpkeys_types.h>
#endif

#ifndef KPKEYS_PKEY_PGTABLES
#define KPKEYS_PKEY_PGTABLES	1
#endif

#if defined(CONFIG_ARCH_HAS_KPKEYS) && !defined(__ASSEMBLY__)

enum kpkeys_ctx {
	KPKEYS_CTX_DEFAULT = 0,
	KPKEYS_CTX_PGTABLES,
	KPKEYS_CTX_COUNT,
};

struct kpkeys_state {
	bool entered_context;
	struct arch_kpkeys_state arch_state;
};

#endif /* CONFIG_ARCH_HAS_KPKEYS && !__ASSEMBLY__ */

#endif /* _LINUX_KPKEYS_TYPES_H */
