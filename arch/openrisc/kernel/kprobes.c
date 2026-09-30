// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Kernel probes (KProbes) for OpenRISC
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * OpenRISC implementation:
 * Copyright (C) 2026 Sahil Siddiq <sahilcdq0@gmail.com>
 */

#include <linux/kprobes.h>
#include <asm/cpuinfo.h>
#include <asm/insn-def.h>
#include <asm/text-patching.h>

DEFINE_PER_CPU(struct kprobe *, current_kprobe);
DEFINE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

#define KPROBE_BP_INSN		__emit_trap(BRK_KPROBE_BP)
#define KPROBE_SSTEPBP_INSN	__emit_trap(BRK_KPROBE_SSTEPBP)

static bool insn_not_supported(kprobe_opcode_t insn)
{
	switch (insn) {
	case OPENRISC_INSN_CSYNC:
	case OPENRISC_INSN_MSYNC:
	case OPENRISC_INSN_PSYNC:
		return true;
	}

	if ((insn & OPENRISC_MASK_TRAP) == OPENRISC_INSN_TRAP)
		return true;

	if ((insn & OPENRISC_MASK_SYS) == OPENRISC_INSN_SYS)
		return true;

	if ((insn & OPENRISC_MASK_MACRC) == OPENRISC_INSN_MACRC)
		return true;

	unsigned char opcode = get_6_bit_opcode(insn);

	switch (opcode) {
	case OPENRISC_OPCODE_RFE:
	case OPENRISC_OPCODE_LWA:
	case OPENRISC_OPCODE_MFSPR:
	case OPENRISC_OPCODE_MTSPR:
	case OPENRISC_OPCODE_SWA:
		return true;
	}

	return false;
}
NOKPROBE_SYMBOL(insn_not_supported)

static bool is_branch_insn(kprobe_opcode_t insn)
{
	unsigned char opcode = get_6_bit_opcode(insn);

	switch (opcode) {
	case OPENRISC_OPCODE_BF:
	case OPENRISC_OPCODE_BNF:
	case OPENRISC_OPCODE_J:
	case OPENRISC_OPCODE_JAL:
	case OPENRISC_OPCODE_JALR:
	case OPENRISC_OPCODE_JR:
		return true;
	}

	return false;
}
NOKPROBE_SYMBOL(is_branch_insn)

static bool is_pc_insn(kprobe_opcode_t insn)
{
	unsigned char opcode = get_6_bit_opcode(insn);

	if (opcode == OPENRISC_OPCODE_ADRP)
		return true;

	return false;
}
NOKPROBE_SYMBOL(is_pc_insn)

static bool insns_need_simulation(kprobe_opcode_t insn, bool *exec_delay_slot)
{
	*exec_delay_slot = false;

	if (is_branch_insn(insn)) {
		if (static_branch_likely(&delay_slot))
			*exec_delay_slot = true;
		return true;
	}

	if (is_pc_insn(insn))
		return true;

	return false;
}
NOKPROBE_SYMBOL(insns_need_simulation)

int arch_prepare_kprobe(struct kprobe *p)
{
	kprobe_opcode_t insn;
	kprobe_opcode_t prev_insn, next_insn;
	bool exec_delay_slot = false;

	/* Attempt to probe at unaligned address */
	if ((unsigned long)p->addr & 0x3)
		return -EILSEQ;

	p->opcode = *p->addr;
	insn = p->opcode;

	if (insn_not_supported(insn)) {
		pr_notice("Can't insert KProbe at blacklisted instruction.\n");
		return -EINVAL;
	}

	if (copy_from_kernel_nofault(&prev_insn, p->addr - 1,
			OPENRISC_INSN_SIZE) == 0 && is_branch_insn(prev_insn)) {
		if (static_branch_likely(&delay_slot)) {
			pr_notice("Can't insert KProbe in delay slot.\n");
			return -EINVAL;
		}
	}

	if (copy_from_kernel_nofault(&next_insn, p->addr + 1, OPENRISC_INSN_SIZE) == 0 &&
	    insns_need_simulation(next_insn, &exec_delay_slot) && is_branch_insn(insn)) {
		if (static_branch_likely(&delay_slot)) {
			pr_notice("Can't insert KProbe if following instruction is in a delay slot and depends on PC.\n");
			return -EINVAL;
		}
	}

	if (insns_need_simulation(insn, &exec_delay_slot) && !exec_delay_slot) {
		p->ainsn.insn = NULL;
	} else {
		/*
		 * Single step probed instruction or, in case of branch instructions, single
		 * step instruction in delay slot.
		 */
		p->ainsn.insn = get_insn_slot();
		if (!p->ainsn.insn)
			return -ENOMEM;

		if (exec_delay_slot)
			patch_insn_write(p->ainsn.insn, *(p->addr + 1));
		else
			patch_insn_write(p->ainsn.insn, p->opcode);
		patch_insn_write(&p->ainsn.insn[1], KPROBE_SSTEPBP_INSN);
	}

	return 0;
}
NOKPROBE_SYMBOL(arch_prepare_kprobe);

void arch_arm_kprobe(struct kprobe *p)
{
	patch_insn_write(p->addr, KPROBE_BP_INSN);
	flush_insn_slot(p);
}
NOKPROBE_SYMBOL(arch_arm_kprobe);

void arch_disarm_kprobe(struct kprobe *p)
{
	patch_insn_write(p->addr, p->opcode);
	flush_insn_slot(p);
}
NOKPROBE_SYMBOL(arch_disarm_kprobe);

void arch_remove_kprobe(struct kprobe *p)
{
	if (p->ainsn.insn) {
		free_insn_slot(p->ainsn.insn, 0);
		p->ainsn.insn = NULL;
	}
}
NOKPROBE_SYMBOL(arch_remove_kprobe);

static void set_current_kprobe(struct kprobe *p)
{
	__this_cpu_write(current_kprobe, p);
}
NOKPROBE_SYMBOL(set_current_kprobe);

static void save_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->prev_kprobe.kp = kprobe_running();
	kcb->prev_kprobe.status = kcb->kprobe_status;
	kcb->prev_kprobe.restore_pc = kcb->restore_pc;
}
NOKPROBE_SYMBOL(save_previous_kprobe);

static void restore_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->kprobe_status = kcb->prev_kprobe.status;
	kcb->restore_pc = kcb->prev_kprobe.restore_pc;
	set_current_kprobe(kcb->prev_kprobe.kp);
}
NOKPROBE_SYMBOL(restore_previous_kprobe);

static void post_kprobe_handler(struct kprobe *cur, struct kprobe_ctlblk *kcb,
				struct pt_regs *regs)
{
	instruction_pointer_set(regs, kcb->restore_pc);

	if (kcb->kprobe_status == KPROBE_REENTER) {
		restore_previous_kprobe(kcb);
		return;
	}

	kcb->kprobe_status = KPROBE_HIT_SSDONE;

	if (cur->post_handler)
		cur->post_handler(cur, regs, 0);

	reset_current_kprobe();
	preempt_enable_no_resched();
}
NOKPROBE_SYMBOL(post_kprobe_handler);

static void setup_singlestep(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb, int reenter)
{
	kprobe_opcode_t insn;

	if (reenter) {
		save_previous_kprobe(kcb);
		set_current_kprobe(p);
		kcb->kprobe_status = KPROBE_REENTER;
	} else {
		kcb->kprobe_status = KPROBE_HIT_SS;
	}

	/* Emulate instruction if required. */
	insn = p->opcode;
	if (is_branch_insn(insn)) {
		simulate_branch(regs, insn);
		/* Save target addr before updating PC to SSOL slot */
		kcb->restore_pc = regs->pc;
	} else if (is_pc_insn(insn)) {
		simulate_pc(regs, insn);
		kcb->restore_pc = regs->pc;
		/* No SSOL is required here, so call post-process immediately. */
		post_kprobe_handler(p, kcb, regs);
		return;
	} else
		kcb->restore_pc = regs->pc + OPENRISC_INSN_SIZE;

	if (p->ainsn.insn) {
		/* Disable IRQs before single stepping */
		local_irq_save(kcb->irq_flags);
		instruction_pointer_set(regs, (unsigned long)p->ainsn.insn);
	} else {
		/*
		 * The instruction is a branch but delay slots are disabled.
		 * Simply call the post-handler.
		 */
		post_kprobe_handler(p, kcb, regs);
	}
}
NOKPROBE_SYMBOL(setup_singlestep);

static bool reenter_kprobe(struct kprobe *p, struct pt_regs *regs,
			   struct kprobe_ctlblk *kcb)
{
	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SS:
	case KPROBE_HIT_SSDONE:
	case KPROBE_HIT_ACTIVE:
		kprobes_inc_nmissed_count(p);
		setup_singlestep(p, regs, kcb, 1);
		break;
	case KPROBE_REENTER:
		pr_warn("Unrecoverable KProbe detected.\n");
		dump_kprobe(p);
		WARN_ON_ONCE(1);
		return false;
	default:
		WARN_ON(1);
		return false;
	}

	return true;
}
NOKPROBE_SYMBOL(reenter_kprobe);

bool kprobe_breakpoint_handler(struct pt_regs *regs)
{
	struct kprobe *p, *curr_kprobe;
	struct kprobe_ctlblk *kcb;
	kprobe_opcode_t *addr = (kprobe_opcode_t *)regs->pc;
	kprobe_opcode_t insn;

	/*
	 * We don't want to be preempted for the entire
	 * duration of kprobe processing.
	 */
	preempt_disable();

	kcb = get_kprobe_ctlblk();
	curr_kprobe = kprobe_running();
	p = get_kprobe(addr);
	if (p) {
		if (curr_kprobe) {
			if (reenter_kprobe(p, regs, kcb))
				return true;
		} else {
			/* Probe hit */
			set_current_kprobe(p);
			kcb->kprobe_status = KPROBE_HIT_ACTIVE;
			/*
			 * If there's no pre-handler or it returns 0, continue
			 * processing the KProbe as usual. A non-zero return
			 * value implies the saved registers have been modified
			 * and the execution path might deviate from what's
			 * expected. Reset the KProbe and return.
			 */
			if (!p->pre_handler || !p->pre_handler(p, regs)) {
				setup_singlestep(p, regs, kcb, 0);
			} else {
				reset_current_kprobe();
				preempt_enable_no_resched();
			}
			return true;
		}
	}

	/*
	 * The breakpoint instruction was removed right after we hit it
	 * possibly by another cpu. If the original instruction was also
	 * a trap instruction then return to the trap handler for further
	 * processing, else no further processing is required.
	 */
	if (get_kernel_nofault(insn, addr) < 0
		|| (*addr & OPENRISC_MASK_TRAP) != OPENRISC_INSN_TRAP) {
		preempt_enable_no_resched();
		return true;
	}

	preempt_enable_no_resched();
	return false;
}
NOKPROBE_SYMBOL(kprobe_breakpoint_handler);

bool kprobe_singlestep_handler(struct pt_regs *regs)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();
	unsigned long addr = instruction_pointer(regs);

	if (cur && (kcb->kprobe_status & (KPROBE_HIT_SS | KPROBE_REENTER)) &&
	    ((unsigned long)&cur->ainsn.insn[1] == addr)) {
		/* Single stepping has been completed. Enable IRQs. */
		regs->sr |= kcb->irq_flags;
		local_irq_restore(kcb->irq_flags);
		post_kprobe_handler(cur, kcb, regs);
		return true;
	}

	return false;
}
NOKPROBE_SYMBOL(kprobe_singlestep_handler);

int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SS:
	case KPROBE_REENTER:
		/*
		 * The instruction being single-stepped caused a page fault.
		 * Reset the current KProbe and set PC to the probe's address.
		 * Then allow the page fault handler to continue as usual.
		 */
		instruction_pointer_set(regs, (unsigned long)cur->addr);

		if (kcb->kprobe_status == KPROBE_REENTER) {
			restore_previous_kprobe(kcb);
		} else {
			regs->sr |= kcb->irq_flags;
			local_irq_restore(kcb->irq_flags);
			reset_current_kprobe();
			preempt_enable_no_resched();
		}
		break;
	}

	return 0;
}
NOKPROBE_SYMBOL(kprobe_fault_handler);

int __init arch_init_kprobes(void)
{
	return 0;
}
