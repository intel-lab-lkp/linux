// SPDX-License-Identifier: GPL-2.0
/*
 * Emulation of the V9 casa compare-and-swap for V8 CPUs.
 *
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 *
 * sun4m implements no compare-and-swap at all, only ldstub and swap, and
 * casa is optional on LEON. Where it is missing, casa is an undefined opcode
 * and raises illegal_instruction (tt 0x02).
 *
 * Both immediate ASIs a compiler emits are accepted: 0x80, the V9 primary
 * address space, and 0x0A, which gcc -mcpu=leon3 uses. The opcode is
 * unimplemented wherever this runs, so the ASI carries no distinction the
 * emulated CAS has to honour.
 *
 * The register-ASI form is not emulated: it reads %asi, which V8 does not
 * have. casxa is a 64-bit CAS and is meaningless on a 32-bit CPU.
 *
 * The CAS takes the futex ops' lock, so it is atomic against them and
 * against itself, but not against a plain store to the same word.
 */

#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <asm/futex.h>
#include <asm/ptrace.h>

#include "entry.h"
#include "unimp_32.h"

#define CASA_OP		3	/* insn[31:30] */
#define CASA_OP3	0x3c	/* insn[24:19] */

#define ASI_P		0x80
#define ASI_LEON	0x0a

/* A fault returns -EFAULT, and the caller pages the word in. */
static int cas_try(u32 __user *uaddr, u32 cmp, u32 newval, u32 *oldval)
{
	int ret;

	pagefault_disable();
	ret = futex_atomic_cmpxchg_inatomic(oldval, uaddr, cmp, newval);
	pagefault_enable();
	return ret;
}

/*
 * Returns 1 if the trap was consumed, either by emulating the CAS and
 * advancing the PC or by delivering a fault signal for a bad address, and 0
 * if this is not a casa we emulate, in which case the caller raises SIGILL.
 */
int try_emulate_casa(struct pt_regs *regs, unsigned int insn)
{
	unsigned int rd, rs1, rs2, asi;
	unsigned long addr;
	u32 cmp, newval, old;
	int fault = 0, ret;

	if ((insn >> 30) != CASA_OP)
		return 0;
	if (((insn >> 19) & 0x3f) != CASA_OP3)
		return 0;
	if (insn & (1 << 13))			/* i == 1: register ASI */
		return 0;
	asi = (insn >> 5) & 0xff;
	if (asi != ASI_P && asi != ASI_LEON)
		return 0;

	rd = (insn >> 25) & 0x1f;
	rs1 = (insn >> 14) & 0x1f;
	rs2 = insn & 0x1f;

	unimp_flush_windows(rs1, rs2, rd);

	addr = unimp_get_reg(rs1, regs, &fault);
	cmp = unimp_get_reg(rs2, regs, &fault);
	newval = unimp_get_reg(rd, regs, &fault);
	if (fault)
		goto sigsegv;

	if (addr & 3) {				/* CAS is naturally aligned */
		send_sig_fault(SIGBUS, BUS_ADRALN, (void __user *)addr, current);
		return 1;
	}
	if (!access_ok((void __user *)addr, sizeof(u32)))
		goto sigsegv;

	while ((ret = cas_try((u32 __user *)addr, cmp, newval, &old)) == -EFAULT) {
		/*
		 * Page the word in writably and retry, as
		 * fault_in_user_writeable() does.
		 */
		struct mm_struct *mm = current->mm;
		int fx;

		mmap_read_lock(mm);
		fx = fixup_user_fault(mm, addr, FAULT_FLAG_WRITE, NULL);
		mmap_read_unlock(mm);
		if (fx)
			goto sigsegv;
	}

	if (unimp_put_reg(rd, old, regs))
		goto sigsegv;

	regs->pc = regs->npc;
	regs->npc += 4;
	return 1;

sigsegv:
	send_sig_fault(SIGSEGV, SEGV_MAPERR, (void __user *)addr, current);
	return 1;
}
