/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_EXCEPTION_H
#define __ASM_EXCEPTION_H

#include <asm/ptrace.h>

asmlinkage void noinstr do_fpe(struct pt_regs *regs, unsigned long fcsr);
asmlinkage void noinstr do_ade(struct pt_regs *regs);
asmlinkage void noinstr do_ale(struct pt_regs *regs);
asmlinkage void noinstr do_bce(struct pt_regs *regs);
asmlinkage void noinstr do_bp(struct pt_regs *regs);
asmlinkage void noinstr do_watch(struct pt_regs *regs);
asmlinkage void noinstr do_ri(struct pt_regs *regs);
asmlinkage void noinstr do_fpu(struct pt_regs *regs);
asmlinkage void noinstr do_lsx(struct pt_regs *regs);
asmlinkage void noinstr do_lasx(struct pt_regs *regs);
asmlinkage void noinstr do_lbt(struct pt_regs *regs);
asmlinkage void noinstr do_reserved(struct pt_regs *regs);
asmlinkage void cache_parity_error(void);
asmlinkage void noinstr handle_loongarch_irq(struct pt_regs *regs);
asmlinkage void noinstr do_vint(struct pt_regs *regs, unsigned long sp);
asmlinkage void __kprobes do_page_fault(struct pt_regs *regs,
				unsigned long write, unsigned long address);

#endif	/* __ASM_EXCEPTION_H */
