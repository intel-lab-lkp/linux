// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic return hook for OpenRISC. Depends on KProbes.
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * OpenRISC implementation:
 * Copyright (C) 2026 Sahil Siddiq <sahilcdq0@gmail.com>
 */

#include <linux/kprobes.h>
#include <linux/rethook.h>
#include "rethook.h"

unsigned long arch_rethook_trampoline_callback(struct pt_regs *regs)
{
	return rethook_trampoline_handler(regs, regs->gpr2);
}
NOKPROBE_SYMBOL(arch_rethook_trampoline_callback)

void arch_rethook_prepare(struct rethook_node *rhn, struct pt_regs *regs, bool mcount)
{
	/* Store original return address and frame pointer */
	rhn->ret_addr = regs->gpr9;
	rhn->frame = regs->gpr2;

	/* Set return address to trampoline */
	regs->gpr9 = (unsigned long)arch_rethook_trampoline;
}
NOKPROBE_SYMBOL(arch_rethook_prepare)
