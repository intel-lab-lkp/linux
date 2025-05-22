/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Huawei Ltd.
 */
#ifndef __ASM_EXCEPTION_MASK_H
#define __ASM_EXCEPTION_MASK_H

#include <asm/ptrace.h>
#include <asm/sysreg.h>

#define DAIF_PROCCTX		0
#define DAIF_PROCCTX_NOIRQ	(PSR_I_BIT | PSR_F_BIT)
#define DAIF_ERRCTX		(PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)
#define DAIF_MASK		(PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)

union cpu_exception_mask {
	unsigned long flags;
	struct {
		unsigned long pmr : 8;		// SYS_ICC_PMR_EL1
		unsigned long daif : 10;	// PSTATE.DAIF at bits[6-9]
		unsigned long allint : 14;	// PSTATE.ALLINT at bits[13]
	} fields;
};

struct cpu_exception_mask_handler {
	void (*mask)(void);             // mask all exception and interrupt
	unsigned long (*save)(void);    // save exception and interrupt masks
	void (*restore)(unsigned long flags); // restore exception from given masks
};

extern const union cpu_exception_mask procctx;
extern const union cpu_exception_mask procctx_noirq;
extern const union cpu_exception_mask errctx;
extern struct cpu_exception_mask_handler *cpu_exception;
int set_exception_mask_handler(int type);

/*
 * The exception masking steps for exception entry and exit:
 *
 * [EL0 Sync]
 *	el0_sync_entry_exception_mask()
 *	...
 *	do_resume_notify()
 *	el0_common_exit_exception_mask()
 *
 * [EL0 IRQ & FIQ]
 *	irq_common_entry_exception_mask()
 *	...
 *	do_resume_notify()
 *	el0_common_exit_exception_mask()
 *
 * [EL0 serror]
 *	serror_entry_exception_mask()
 *	...
 *	el0_serror_exit_exception_mask()
 *	do_resume_notify()
 *	el0_exit_common_exception_mask()
 *
 * [EL1 Sync]
 *	el1_sync_entry_exception_mask()
 *	...
 *	el1_sync_exit_exception_mask()
 *
 * [EL1 IRQ & FIQ]
 *	irq_common_entry_exception_mask()
 *
 * [EL1 Serror]
 *	serror_entry_exception_mask()
 */
static inline void el0_sync_entry_exception_mask(void)
{
	cpu_exception->restore(procctx.flags);
}

static inline void irq_common_entry_exception_mask(void)
{
	/* only mask normal interrupts and NMIs*/
	asm volatile ("msr daifclr, #0xc" : : : "memory");
}

static inline void serror_entry_exception_mask(void)
{
	cpu_exception->restore(errctx.flags);
}

static inline void el0_serror_exit_exception_mask(void)
{
	cpu_exception->restore(procctx.flags);
}

static inline void el0_common_exit_exception_mask(void)
{
	cpu_exception->mask();
}

static inline void el1_sync_entry_exception_mask(struct pt_regs *regs)
{
	union cpu_exception_mask mask;

	mask.fields.pmr = regs->pmr;
	mask.fields.daif = regs->pstate & DAIF_MASK;

	cpu_exception->restore(mask.flags);
}

static inline void el1_sync_exit_exception_mask(void)
{
	cpu_exception->mask();
}

static inline unsigned long local_exception_save(void)
{
	unsigned long flags;

	flags = cpu_exception->save();
	cpu_exception->mask();

	return flags;
}

static inline unsigned long local_exception_save_flags(void)
{
	return cpu_exception->save();
}

/* mask/save/unmask/restore all exceptions, including interrupts. */
static inline void local_exception_mask(void)
{
	cpu_exception->mask();
}

static inline void local_exception_restore(unsigned long flags)
{
	cpu_exception->restore(flags);
}

#endif
