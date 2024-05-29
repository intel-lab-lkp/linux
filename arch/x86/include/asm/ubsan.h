/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UBSAN_H
#define _ASM_X86_UBSAN_H

/*
 * Clang Undefined Behavior Sanitizer trap mode support.
 */
#include <linux/bug.h>
#include <linux/ubsan.h>
#include <asm/ptrace.h>

#ifdef CONFIG_UBSAN_TRAP
enum bug_trap_type handle_ubsan_failure(struct pt_regs *regs, int insn);
#else
static inline enum bug_trap_type handle_ubsan_failure(struct pt_regs *regs, int insn)
{
	return BUG_TRAP_TYPE_NONE;
}
#endif /* CONFIG_UBSAN_TRAP */

#endif /* _ASM_X86_UBSAN_H */
