// SPDX-License-Identifier: GPL-2.0
/*
 * arch/sparc/kernel/traps.c
 *
 * Copyright 1995, 2008 David S. Miller (davem@davemloft.net)
 * Copyright 2000 Jakub Jelinek (jakub@redhat.com)
 */

/*
 * I hate traps on the sparc, grrr...
 */

#include <linux/cpu.h>
#include <linux/sched/mm.h>
#include <linux/sched/debug.h>
#include <linux/mm_types.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/smp.h>
#include <linux/kdebug.h>
#include <linux/export.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>

#include <asm/delay.h>
#include <asm/ptrace.h>
#include <asm/oplib.h>
#include <asm/page.h>
#include <asm/unistd.h>
#include <asm/traps.h>

#include "entry.h"
#include "kernel.h"

/* #define TRAP_DEBUG */

static void instruction_dump(unsigned long *pc)
{
	int i;
	
	if((((unsigned long) pc) & 3))
                return;

	for(i = -3; i < 6; i++)
		printk("%c%08lx%c",i?' ':'<',pc[i],i?' ':'>');
	printk("\n");
}

#define __SAVE __asm__ __volatile__("save %sp, -0x40, %sp\n\t")
#define __RESTORE __asm__ __volatile__("restore %g0, %g0, %g0\n\t")

void __noreturn die_if_kernel(char *str, struct pt_regs *regs)
{
	static int die_counter;
	int count = 0;

	/* Amuse the user. */
	printk(
"              \\|/ ____ \\|/\n"
"              \"@'/ ,. \\`@\"\n"
"              /_| \\__/ |_\\\n"
"                 \\__U_/\n");

	printk("%s(%d): %s [#%d]\n", current->comm, task_pid_nr(current), str, ++die_counter);
	show_regs(regs);
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);

	__SAVE; __SAVE; __SAVE; __SAVE;
	__SAVE; __SAVE; __SAVE; __SAVE;
	__RESTORE; __RESTORE; __RESTORE; __RESTORE;
	__RESTORE; __RESTORE; __RESTORE; __RESTORE;

	{
		struct reg_window32 *rw = (struct reg_window32 *)regs->u_regs[UREG_FP];

		/* Stop the back trace when we hit userland or we
		 * find some badly aligned kernel stack. Set an upper
		 * bound in case our stack is trashed and we loop.
		 */
		while(rw					&&
		      count++ < 30				&&
                      (((unsigned long) rw) >= PAGE_OFFSET)	&&
		      !(((unsigned long) rw) & 0x7)) {
			printk("Caller[%08lx]: %pS\n", rw->ins[7],
			       (void *) rw->ins[7]);
			rw = (struct reg_window32 *)rw->ins[6];
		}
	}
	printk("Instruction DUMP:");
	instruction_dump ((unsigned long *) regs->pc);
	make_task_dead((regs->psr & PSR_PS) ? SIGKILL : SIGSEGV);
}

void do_hw_interrupt(struct pt_regs *regs, unsigned long type)
{
	if(type < 0x80) {
		/* Sun OS's puke from bad traps, Linux survives! */
		printk("Unimplemented Sparc TRAP, type = %02lx\n", type);
		die_if_kernel("Whee... Hello Mr. Penguin", regs);
	}	

	if(regs->psr & PSR_PS)
		die_if_kernel("Kernel bad trap", regs);

	force_sig_fault_trapno(SIGILL, ILL_ILLTRP,
			       (void __user *)regs->pc, type - 0x80);
}

/* Some SPARC V8 implementations raise illegal_instruction instead of
 * completing UDIV/SDIV for every operand.  Finish the divide in software and
 * step over it rather than killing the process for an instruction the
 * architecture defines.
 */
#define DIV_OP3_UDIV	0x0e
#define DIV_OP3_SDIV	0x0f
#define DIV_OP3_UDIVCC	0x1e
#define DIV_OP3_SDIVCC	0x1f

/* %l and %i live in the register file until the windows spill, so force them
 * out before reading them off the stack.  Without this we read stale memory and
 * compute a confidently wrong quotient.
 */
static inline void div_maybe_flush_windows(unsigned int rs1, unsigned int rs2,
					   unsigned int rd)
{
	if (rs2 >= 16 || rs1 >= 16 || rd >= 16) {
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
}

/* The window is only reachable if %fp is a properly aligned user address. */
static struct reg_window32 __user *div_user_window(struct pt_regs *regs)
{
	unsigned long fp = regs->u_regs[UREG_FP];

	if (fp & (sizeof(unsigned long) * 2 - 1))
		return NULL;
	return (struct reg_window32 __user *) fp;
}

/* Where a windowed register was spilled, or NULL if unreachable.  %fp may be
 * mapped while the slot itself lands on the next page, so callers report this
 * address rather than %fp on failure.
 */
static unsigned long __user *div_reg_slot(unsigned int reg,
					 struct pt_regs *regs)
{
	struct reg_window32 __user *win = div_user_window(regs);

	if (!win)
		return NULL;
	if (reg < 24)
		return &win->locals[reg - 16];
	return &win->ins[reg - 24];
}

static int div_fetch_reg(unsigned int reg, struct pt_regs *regs,
			 unsigned long *val, void __user **fault_at)
{
	unsigned long __user *slot;

	if (reg < 16) {
		*val = reg ? regs->u_regs[reg] : 0;
		return 0;
	}

	/* %l and %i live in the register window on the user stack. */
	slot = div_reg_slot(reg, regs);
	if (!slot) {
		*fault_at = (void __user *)regs->u_regs[UREG_FP];
		return -EFAULT;
	}
	if (get_user(*val, slot)) {
		*fault_at = (void __user *)slot;
		return -EFAULT;
	}
	return 0;
}

static int div_store_reg(unsigned int reg, struct pt_regs *regs,
			 unsigned long val, void __user **fault_at)
{
	unsigned long __user *slot;

	if (reg == 0)			/* %g0 is hardwired to zero */
		return 0;
	if (reg < 16) {
		regs->u_regs[reg] = val;
		return 0;
	}

	slot = div_reg_slot(reg, regs);
	if (!slot) {
		*fault_at = (void __user *)regs->u_regs[UREG_FP];
		return -EFAULT;
	}
	if (put_user(val, slot)) {
		*fault_at = (void __user *)slot;
		return -EFAULT;
	}
	return 0;
}

/* DIV_NOT_MINE: not one of ours, the caller must still raise SIGILL.
 * DIV_DONE:     emulated (or turned into SIGFPE); swallow the trap.
 * DIV_FAULT:    it was a divide, but its register window was unreachable.
 */
#define DIV_NOT_MINE	0
#define DIV_DONE	1
#define DIV_FAULT	(-1)

static int emulate_divide(struct pt_regs *regs, unsigned long pc,
			  void __user **fault_at)
{
	unsigned int insn, op3, rd, rs1, rs2;
	unsigned long v1, divisor;
	int is_signed, sets_cc;
	unsigned long result;
	u64 dividend;
	int overflow = 0;

	if (get_user(insn, (unsigned int __user *) pc))
		return DIV_NOT_MINE;

	if ((insn >> 30) != 2)			/* not format 3 */
		return DIV_NOT_MINE;

	op3 = (insn >> 19) & 0x3f;
	switch (op3) {
	case DIV_OP3_UDIV:
		is_signed = 0;
		sets_cc = 0;
		break;
	case DIV_OP3_SDIV:
		is_signed = 1;
		sets_cc = 0;
		break;
	case DIV_OP3_UDIVCC:
		is_signed = 0;
		sets_cc = 1;
		break;
	case DIV_OP3_SDIVCC:
		is_signed = 1;
		sets_cc = 1;
		break;
	default:
		return DIV_NOT_MINE;
	}

	/* Definitely a divide now, so unreachable operands are a fault. */
	rd  = (insn >> 25) & 0x1f;
	rs1 = (insn >> 14) & 0x1f;
	rs2 = (insn & 0x2000) ? 0 : (insn & 0x1f);

	div_maybe_flush_windows(rs1, rs2, rd);

	if (div_fetch_reg(rs1, regs, &v1, fault_at))
		return DIV_FAULT;

	if (insn & 0x2000) {			/* i=1, simm13 */
		divisor = (unsigned long)(long)((int)(insn << 19) >> 19);
	} else {
		if (div_fetch_reg(rs2, regs, &divisor, fault_at))
			return DIV_FAULT;
	}

	if (divisor == 0) {
		/* Hardware raises division_by_zero here; keep that contract. */
		send_sig_fault(SIGFPE, FPE_INTDIV, (void __user *)pc, current);
		return DIV_DONE;
	}

	dividend = ((u64)regs->y << 32) | (u32)v1;

	if (!is_signed) {
		u64 q = dividend / (u32)divisor;

		if (q > 0xffffffffULL) {
			q = 0xffffffffULL;	/* V8: clamp on overflow */
			overflow = 1;
		}
		result = (unsigned long)q;
	} else {
		s64 sdividend = (s64)dividend;
		s32 sdivisor = (s32)divisor;
		s64 q;

		/* S64_MIN / -1 wants 2^63, which is not representable, and in
		 * C the expression is undefined rather than merely out of
		 * range.  Saturate directly instead of dividing.
		 */
		if (sdividend == S64_MIN && sdivisor == -1) {
			q = (s64)0x7fffffff;
			overflow = 1;
		} else {
			q = sdividend / sdivisor;
		}

		if (q > (s64)0x7fffffff) {
			q = 0x7fffffff;
			overflow = 1;
		} else if (q < -(s64)0x80000000LL) {
			q = -(s64)0x80000000LL;
			overflow = 1;
		}
		result = (unsigned long)(s32)q;
	}

	if (div_store_reg(rd, regs, result, fault_at))
		return DIV_FAULT;

	if (sets_cc) {
		unsigned long psr = regs->psr;

		psr &= ~(PSR_N | PSR_Z | PSR_V | PSR_C);
		if ((s32)result < 0)
			psr |= PSR_N;
		if (result == 0)
			psr |= PSR_Z;
		if (overflow)
			psr |= PSR_V;
		regs->psr = psr;
	}

	/* Step over the instruction we just performed. */
	regs->pc = regs->npc;
	regs->npc = regs->npc + 4;
	return DIV_DONE;
}

void do_illegal_instruction(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			    unsigned long psr)
{
	void __user *fault_at = (void __user *)pc;

	if(psr & PSR_PS)
		die_if_kernel("Kernel illegal instruction", regs);
#ifdef TRAP_DEBUG
	printk("Ill instr. at pc=%08lx instruction is %08lx\n",
	       regs->pc, *(unsigned long *)regs->pc);
#endif

	switch (emulate_divide(regs, pc, &fault_at)) {
	case DIV_DONE:
		return;
	case DIV_FAULT:
		/* The instruction was legal; the stack it spilled to was not. */
		send_sig_fault(SIGSEGV, SEGV_MAPERR, fault_at, current);
		return;
	}

	send_sig_fault(SIGILL, ILL_ILLOPC, (void __user *)pc, current);
}

void do_priv_instruction(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			 unsigned long psr)
{
	if(psr & PSR_PS)
		die_if_kernel("Penguin instruction from Penguin mode??!?!", regs);
	send_sig_fault(SIGILL, ILL_PRVOPC, (void __user *)pc, current);
}

/* XXX User may want to be allowed to do this. XXX */

void do_memaccess_unaligned(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			    unsigned long psr)
{
	if(regs->psr & PSR_PS) {
		printk("KERNEL MNA at pc %08lx npc %08lx called by %08lx\n", pc, npc,
		       regs->u_regs[UREG_RETPC]);
		die_if_kernel("BOGUS", regs);
		/* die_if_kernel("Kernel MNA access", regs); */
	}
#if 0
	show_regs (regs);
	instruction_dump ((unsigned long *) regs->pc);
	printk ("do_MNA!\n");
#endif
	send_sig_fault(SIGBUS, BUS_ADRALN,
		       /* FIXME: Should dig out mna address */ (void *)0,
		       current);
}

static unsigned long init_fsr = 0x0UL;
static unsigned long init_fregs[32] __attribute__ ((aligned (8))) =
                { ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL,
		  ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL,
		  ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL,
		  ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL, ~0UL };

void do_fpd_trap(struct pt_regs *regs, unsigned long pc, unsigned long npc,
		 unsigned long psr)
{
	/* Sanity check... */
	if(psr & PSR_PS)
		die_if_kernel("Kernel gets FloatingPenguinUnit disabled trap", regs);

	put_psr(get_psr() | PSR_EF);    /* Allow FPU ops. */
	regs->psr |= PSR_EF;
#ifndef CONFIG_SMP
	if(last_task_used_math == current)
		return;
	if(last_task_used_math) {
		/* Other processes fpu state, save away */
		struct task_struct *fptask = last_task_used_math;
		fpsave(&fptask->thread.float_regs[0], &fptask->thread.fsr,
		       &fptask->thread.fpqueue[0], &fptask->thread.fpqdepth);
	}
	last_task_used_math = current;
	if(used_math()) {
		fpload(&current->thread.float_regs[0], &current->thread.fsr);
	} else {
		/* Set initial sane state. */
		fpload(&init_fregs[0], &init_fsr);
		set_used_math();
	}
#else
	if(!used_math()) {
		fpload(&init_fregs[0], &init_fsr);
		set_used_math();
	} else {
		fpload(&current->thread.float_regs[0], &current->thread.fsr);
	}
	set_thread_flag(TIF_USEDFPU);
#endif
}

static unsigned long fake_regs[32] __attribute__ ((aligned (8)));
static unsigned long fake_fsr;
static unsigned long fake_queue[32] __attribute__ ((aligned (8)));
static unsigned long fake_depth;

void do_fpe_trap(struct pt_regs *regs, unsigned long pc, unsigned long npc,
		 unsigned long psr)
{
	static int calls;
	unsigned long fsr;
	int ret = 0;
	int code;
#ifndef CONFIG_SMP
	struct task_struct *fpt = last_task_used_math;
#else
	struct task_struct *fpt = current;
#endif
	put_psr(get_psr() | PSR_EF);
	/* If nobody owns the fpu right now, just clear the
	 * error into our fake static buffer and hope it don't
	 * happen again.  Thank you crashme...
	 */
#ifndef CONFIG_SMP
	if(!fpt) {
#else
	if (!test_tsk_thread_flag(fpt, TIF_USEDFPU)) {
#endif
		fpsave(&fake_regs[0], &fake_fsr, &fake_queue[0], &fake_depth);
		regs->psr &= ~PSR_EF;
		return;
	}
	fpsave(&fpt->thread.float_regs[0], &fpt->thread.fsr,
	       &fpt->thread.fpqueue[0], &fpt->thread.fpqdepth);
#ifdef DEBUG_FPU
	printk("Hmm, FP exception, fsr was %016lx\n", fpt->thread.fsr);
#endif

	switch ((fpt->thread.fsr & 0x1c000)) {
	/* switch on the contents of the ftt [floating point trap type] field */
#ifdef DEBUG_FPU
	case (1 << 14):
		printk("IEEE_754_exception\n");
		break;
#endif
	case (2 << 14):  /* unfinished_FPop (underflow & co) */
	case (3 << 14):  /* unimplemented_FPop (quad stuff, maybe sqrt) */
		ret = do_mathemu(regs, fpt);
		break;
#ifdef DEBUG_FPU
	case (4 << 14):
		printk("sequence_error (OS bug...)\n");
		break;
	case (5 << 14):
		printk("hardware_error (uhoh!)\n");
		break;
	case (6 << 14):
		printk("invalid_fp_register (user error)\n");
		break;
#endif /* DEBUG_FPU */
	}
	/* If we successfully emulated the FPop, we pretend the trap never happened :-> */
	if (ret) {
		fpload(&current->thread.float_regs[0], &current->thread.fsr);
		return;
	}
	/* nope, better SIGFPE the offending process... */
	       
#ifdef CONFIG_SMP
	clear_tsk_thread_flag(fpt, TIF_USEDFPU);
#endif
	if(psr & PSR_PS) {
		/* The first fsr store/load we tried trapped,
		 * the second one will not (we hope).
		 */
		printk("WARNING: FPU exception from kernel mode. at pc=%08lx\n",
		       regs->pc);
		regs->pc = regs->npc;
		regs->npc += 4;
		calls++;
		if(calls > 2)
			die_if_kernel("Too many Penguin-FPU traps from kernel mode",
				      regs);
		return;
	}

	fsr = fpt->thread.fsr;
	code = FPE_FLTUNK;
	if ((fsr & 0x1c000) == (1 << 14)) {
		if (fsr & 0x10)
			code = FPE_FLTINV;
		else if (fsr & 0x08)
			code = FPE_FLTOVF;
		else if (fsr & 0x04)
			code = FPE_FLTUND;
		else if (fsr & 0x02)
			code = FPE_FLTDIV;
		else if (fsr & 0x01)
			code = FPE_FLTRES;
	}
	send_sig_fault(SIGFPE, code, (void __user *)pc, fpt);
#ifndef CONFIG_SMP
	last_task_used_math = NULL;
#endif
	regs->psr &= ~PSR_EF;
	if(calls > 0)
		calls=0;
}

void handle_tag_overflow(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			 unsigned long psr)
{
	if(psr & PSR_PS)
		die_if_kernel("Penguin overflow trap from kernel mode", regs);
	send_sig_fault(SIGEMT, EMT_TAGOVF, (void __user *)pc, current);
}

void handle_watchpoint(struct pt_regs *regs, unsigned long pc, unsigned long npc,
		       unsigned long psr)
{
#ifdef TRAP_DEBUG
	printk("Watchpoint detected at PC %08lx NPC %08lx PSR %08lx\n",
	       pc, npc, psr);
#endif
	if(psr & PSR_PS)
		panic("Tell me what a watchpoint trap is, and I'll then deal "
		      "with such a beast...");
}

void handle_reg_access(struct pt_regs *regs, unsigned long pc, unsigned long npc,
		       unsigned long psr)
{
#ifdef TRAP_DEBUG
	printk("Register Access Exception at PC %08lx NPC %08lx PSR %08lx\n",
	       pc, npc, psr);
#endif
	force_sig_fault(SIGBUS, BUS_OBJERR, (void __user *)pc);
}

void handle_cp_disabled(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			unsigned long psr)
{
	send_sig_fault(SIGILL, ILL_COPROC, (void __user *)pc, current);
}

void handle_cp_exception(struct pt_regs *regs, unsigned long pc, unsigned long npc,
			 unsigned long psr)
{
#ifdef TRAP_DEBUG
	printk("Co-Processor Exception at PC %08lx NPC %08lx PSR %08lx\n",
	       pc, npc, psr);
#endif
	send_sig_fault(SIGILL, ILL_COPROC, (void __user *)pc, current);
}

void handle_hw_divzero(struct pt_regs *regs, unsigned long pc, unsigned long npc,
		       unsigned long psr)
{
	send_sig_fault(SIGFPE, FPE_INTDIV, (void __user *)pc, current);
}

#ifdef CONFIG_DEBUG_BUGVERBOSE
void do_BUG(const char *file, int line)
{
        // bust_spinlocks(1);   XXX Not in our original BUG()
        printk("kernel BUG at %s:%d!\n", file, line);
}
EXPORT_SYMBOL(do_BUG);
#endif

/* Since we have our mappings set up, on multiprocessors we can spin them
 * up here so that timer interrupts work during initialization.
 */

void trap_init(void)
{
	extern void thread_info_offsets_are_bolixed_pete(void);

	/* Force linker to barf if mismatched */
	if (TI_UWINMASK    != offsetof(struct thread_info, uwinmask) ||
	    TI_TASK        != offsetof(struct thread_info, task) ||
	    TI_FLAGS       != offsetof(struct thread_info, flags) ||
	    TI_CPU         != offsetof(struct thread_info, cpu) ||
	    TI_PREEMPT     != offsetof(struct thread_info, preempt_count) ||
	    TI_SOFTIRQ     != offsetof(struct thread_info, softirq_count) ||
	    TI_HARDIRQ     != offsetof(struct thread_info, hardirq_count) ||
	    TI_KSP         != offsetof(struct thread_info, ksp) ||
	    TI_KPC         != offsetof(struct thread_info, kpc) ||
	    TI_KPSR        != offsetof(struct thread_info, kpsr) ||
	    TI_KWIM        != offsetof(struct thread_info, kwim) ||
	    TI_REG_WINDOW  != offsetof(struct thread_info, reg_window) ||
	    TI_RWIN_SPTRS  != offsetof(struct thread_info, rwbuf_stkptrs) ||
	    TI_W_SAVED     != offsetof(struct thread_info, w_saved))
		thread_info_offsets_are_bolixed_pete();

	/* Attach to the address space of init_task. */
	mmgrab(&init_mm);
	current->active_mm = &init_mm;

	/* NOTE: Other cpus have this done as they are started
	 *       up on SMP.
	 */
}
