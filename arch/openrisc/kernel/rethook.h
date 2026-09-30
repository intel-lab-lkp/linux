/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __OPENRISC_RETHOOK_H
#define __OPENRISC_RETHOOK_H

unsigned long arch_rethook_trampoline_callback(struct pt_regs *regs);
void arch_rethook_prepare(struct rethook_node *rhn, struct pt_regs *regs, bool mcount);

#endif
