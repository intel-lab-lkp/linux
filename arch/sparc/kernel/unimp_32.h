/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 */
#ifndef _SPARC_UNIMP_32_H
#define _SPARC_UNIMP_32_H

#include <linux/uaccess.h>
#include <asm/ptrace.h>

/*
 * Operand access for instructions completed out of do_illegal_instruction().
 *
 * Like maybe_flush_windows() in unaligned_32.c this has to be inlined: the
 * save/restore chain must run in the trapping window, not one level deeper
 * behind a call, or the outermost user window is left un-flushed and
 * try_to_clear_window_buffer() kills the task on its next fork.
 */
static __always_inline void unimp_flush_windows(unsigned int rs1,
						unsigned int rs2,
						unsigned int rd)
{
	if (rs1 >= 16 || rs2 >= 16 || rd >= 16)
		__asm__ __volatile__("save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "save %sp, -0x40, %sp\n\t"
				     "restore; restore; restore; restore;\n\t"
				     "restore; restore; restore;\n\t");
}

/*
 * Globals and outs come from pt_regs, locals and ins from the flushed window
 * on the user stack. %g0 reads as zero.
 */
static inline unsigned long unimp_get_reg(unsigned int reg,
					  struct pt_regs *regs, int *fault)
{
	struct reg_window32 __user *win;
	unsigned long val;

	if (reg < 16)
		return reg ? regs->u_regs[reg] : 0;

	win = (struct reg_window32 __user *)regs->u_regs[UREG_FP];
	if (get_user(val, &win->locals[reg - 16])) {
		*fault = 1;
		return 0;
	}
	return val;
}

/* %g0 discards the result. */
static inline int unimp_put_reg(unsigned int reg, unsigned long val,
				struct pt_regs *regs)
{
	struct reg_window32 __user *win;

	if (reg < 16) {
		if (reg)
			regs->u_regs[reg] = val;
		return 0;
	}
	win = (struct reg_window32 __user *)regs->u_regs[UREG_FP];
	return put_user(val, &win->locals[reg - 16]);
}

#endif /* _SPARC_UNIMP_32_H */
