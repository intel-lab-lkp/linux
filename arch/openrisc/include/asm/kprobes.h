/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * OpenRISC Linux
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * OpenRISC implementation:
 * Copyright (C) 2026 Sahil Siddiq <sahilcdq0@gmail.com>
 */

#ifndef __ASM_OPENRISC_KPROBES_H
#define __ASM_OPENRISC_KPROBES_H

#include <asm-generic/kprobes.h>

#ifdef CONFIG_KPROBES
#include <asm/break.h>
#include <asm/cacheflush.h>

#define	 __ARCH_WANT_KPROBES_INSN_SLOT

struct pt_regs;
struct kprobe;

typedef u32 kprobe_opcode_t;

/*
 * MAX_INSN_SIZE is used as the number of slots in an executable
 * page for single-stepping out of line (SSOL). We need two slots
 * since we single-step using software breakpoints. The probed
 * instruction is placed in the first slot with a breakpoint
 * instruction in the second slot.
 */
#define MAX_INSN_SIZE	2

/* Architecture specific copy of original instruction */
struct arch_specific_insn {
	/* copy of original instruction */
	kprobe_opcode_t *insn;
};

struct prev_kprobe {
	struct kprobe *kp;
	unsigned int status;
	unsigned long restore_pc;
};

/* per-cpu kprobe control block */
struct kprobe_ctlblk {
	unsigned int kprobe_status;
	unsigned long irq_flags;
	/* address of next instruction in case of SSOL */
	unsigned long restore_pc;
	struct prev_kprobe prev_kprobe;
};

#define flush_insn_slot(p)		do { } while (0)
#define kretprobe_blacklist_size	0

void arch_remove_kprobe(struct kprobe *p);
int kprobe_fault_handler(struct pt_regs *regs, int trapnr);
bool kprobe_breakpoint_handler(struct pt_regs *regs);
bool kprobe_singlestep_handler(struct pt_regs *regs);
#else /* !CONFIG_KPROBES */
static inline bool kprobe_breakpoint_handler(struct pt_regs *regs)
{
	return false;
}

static inline bool kprobe_singlestep_handler(struct pt_regs *regs)
{
	return false;
}
#endif /* CONFIG_KPROBES */
#endif /* __ASM_OPENRISC_KPROBES_H */
