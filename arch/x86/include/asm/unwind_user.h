/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_H
#define _ASM_X86_UNWIND_USER_H

#include <linux/unwind_user_types.h>
#include <asm/ptrace.h>

#define ARCH_INIT_USER_FP_FRAME							\
	.cfa_off	= (s32)sizeof(long) *  2,				\
	.ra_off		= (s32)sizeof(long) * -1,				\
	.fp_off		= (s32)sizeof(long) * -2,				\
	.use_fp		= true,

#ifdef CONFIG_IA32_EMULATION

#define ARCH_INIT_USER_COMPAT_FP_FRAME						\
	.cfa_off	= (s32)sizeof(u32)  *  2,				\
	.ra_off		= (s32)sizeof(u32)  * -1,				\
	.fp_off		= (s32)sizeof(u32)  * -2,				\
	.use_fp		= true,

#define in_compat_mode(regs) !user_64bit_mode(regs)

static inline void arch_unwind_user_init(struct unwind_user_state *state,
					 struct pt_regs *regs)
{
	unsigned long cs_base, ss_base;

	if (state->type != UNWIND_USER_TYPE_COMPAT_FP)
		return;

	scoped_guard(irqsave) {
		cs_base = segment_base_address(regs->cs);
		ss_base = segment_base_address(regs->ss);
	}

	state->arch.cs_base = cs_base;
	state->arch.ss_base = ss_base;

	state->ip += cs_base;
	state->sp += ss_base;
	state->fp += ss_base;
}
#define arch_unwind_user_init arch_unwind_user_init

static inline void arch_unwind_user_next(struct unwind_user_state *state)
{
	if (state->type != UNWIND_USER_TYPE_COMPAT_FP)
		return;

	state->ip += state->arch.cs_base;
	state->fp += state->arch.ss_base;
}
#define arch_unwind_user_next arch_unwind_user_next

#endif /* CONFIG_IA32_EMULATION */

#include <asm-generic/unwind_user.h>

#endif /* _ASM_X86_UNWIND_USER_H */
