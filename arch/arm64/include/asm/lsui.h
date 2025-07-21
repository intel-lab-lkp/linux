/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Arm Ltd.
 */
#ifndef __ASM_LSUI_H
#define __ASM_LSUI_H

#define LL_SC_MAX_LOOPS	128 /* What's the largest number you can think of? */

#include <asm/futex_ll_sc_u.h>

#ifdef CONFIG_AS_HAS_LSUI

#define __LSUI_PREAMBLE	".arch_extension lsui\n"

#include <linux/compiler_types.h>
#include <linux/export.h>
#include <linux/stringify.h>
#include <asm/alternative.h>
#include <asm/alternative-macros.h>
#include <asm/cpucaps.h>

#include <asm/futex_lsui.h>

#define __lsui_ll_sc_u_body(op, ...)					\
({									\
	alternative_has_cap_likely(ARM64_HAS_LSUI) ?		\
		__lsui_##op(__VA_ARGS__) :				\
		__ll_sc_u_##op(__VA_ARGS__);				\
})

#else	/* CONFIG_AS_HAS_LSUI */

#define __lsui_ll_sc_u_body(op, ...)		__ll_sc_u_##op(__VA_ARGS__)

#endif	/* CONFIG_AS_HAS_LSUI */
#endif	/* __ASM_LSUI_H */
