/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_TYPES_H
#define _ASM_X86_UNWIND_USER_TYPES_H

#ifdef CONFIG_IA32_EMULATION

struct arch_unwind_user_state {
	unsigned long ss_base;
	unsigned long cs_base;
};
#define arch_unwind_user_state arch_unwind_user_state

#endif /* CONFIG_IA32_EMULATION */

#include <asm-generic/unwind_user_types.h>

#endif /* _ASM_UNWIND_USER_TYPES_H */
