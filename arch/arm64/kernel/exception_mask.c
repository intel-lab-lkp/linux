// SPDX-License-Identifier: GPL-2.0
/*
 * Exception masking code for DAIF, PMR and ALLINT.
 *
 * Copyright (C) 2025 Huawei Ltd.
 */
#include <linux/irqflags.h>

#include <asm/arch_gicv3.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/ptrace.h>
#include <asm/exception_mask.h>

/* unmask all interrupts including interrupts */
const union cpu_exception_mask procctx = {
	.fields.daif = 0,
	.fields.pmr = GIC_PRIO_IRQON,
	.fields.allint = 0,
};

/* only mask normal interrupts */
const union cpu_exception_mask procctx_noirq = {
	.fields.daif = DAIF_PROCCTX_NOIRQ,
	.fields.pmr = GIC_PRIO_IRQOFF,
	.fields.allint = 0,
};

/* mask all interrupts including NMI and Serror */
const union cpu_exception_mask errctx = {
	.fields.daif = DAIF_ERRCTX,
	.fields.pmr = GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET,
	.fields.allint = 1,
};

static void daif_exception_mask(void)
{
	asm volatile(
		"msr	daifset, #0xf\n"
		:
		:
		: "memory");

	trace_hardirqs_off();
}

static unsigned long daif_exception_save(void)
{
	return read_sysreg(daif);
}

static void daif_exception_restore(unsigned long flags)
{
	union cpu_exception_mask mask = { .flags = flags };
	bool irq_disabled = mask.fields.daif & PSR_I_BIT;

	if (!irq_disabled)
		trace_hardirqs_on();

	write_sysreg(mask.fields.daif, daif);

	if (irq_disabled)
		trace_hardirqs_off();
}

static struct cpu_exception_mask_handler daif_handler = {
	.mask		= daif_exception_mask,
	.save		= daif_exception_save,
	.restore	= daif_exception_restore,
};

static void pmr_exception_mask(void)
{
	WARN_ON(system_has_prio_mask_debugging() &&
		(read_sysreg_s(SYS_ICC_PMR_EL1) == (GIC_PRIO_IRQOFF |
						    GIC_PRIO_PSR_I_SET)));

	asm volatile(
		"msr	daifset, #0xf\n"
		:
		:
		: "memory");

	gic_write_pmr(errctx.fields.pmr);

	trace_hardirqs_off();
}

static unsigned long pmr_exception_save(void)
{
	union cpu_exception_mask mask = { .flags = 0UL };

	mask.fields.daif = read_sysreg(daif);
	mask.fields.allint = mask.fields.daif & PSR_A_BIT;

	/* If IRQs are masked with PMR, reflect it in the daif */
	if (read_sysreg_s(SYS_ICC_PMR_EL1) != procctx.fields.pmr) {
		mask.fields.daif |= DAIF_PROCCTX_NOIRQ;
		mask.fields.pmr = mask.fields.allint ?
				errctx.fields.pmr : procctx_noirq.fields.pmr;
	} else {
		mask.fields.pmr = procctx.fields.pmr;
	}

	return mask.flags;
}

static void pmr_exception_restore(unsigned long flags)
{
	union cpu_exception_mask mask = { .flags = flags };
	bool irq_disabled = (mask.fields.daif & PSR_I_BIT);

	WARN_ON(system_has_prio_mask_debugging() &&
		(read_sysreg(daif) & (DAIF_PROCCTX_NOIRQ)) != (DAIF_PROCCTX_NOIRQ));

	if (!irq_disabled) {
		trace_hardirqs_on();
		gic_write_pmr(mask.fields.pmr);
		pmr_sync();
	} else {
		if (!mask.fields.allint)
			mask.fields.daif &= ~DAIF_PROCCTX_NOIRQ;
		/*
		 * There has been concern that the write to daif
		 * might be reordered before this write to PMR.
		 * From the ARM ARM DDI 0487D.a, section D1.7.1
		 * "Accessing PSTATE fields":
		 *   Writes to the PSTATE fields have side-effects on
		 *   various aspects of the PE operation. All of these
		 *   side-effects are guaranteed:
		 *     - Not to be visible to earlier instructions in
		 *       the execution stream.
		 *     - To be visible to later instructions in the
		 *       execution stream
		 *
		 * Also, writes to PMR are self-synchronizing, so no
		 * interrupts with a lower priority than PMR is signaled
		 * to the PE after the write.
		 *
		 * So we don't need additional synchronization here.
		 */
		gic_write_pmr(mask.fields.pmr);
	}

	write_sysreg(mask.fields.daif, daif);

	if (irq_disabled)
		trace_hardirqs_off();
}

static struct cpu_exception_mask_handler pmr_handler = {
	.mask		= pmr_exception_mask,
	.save		= pmr_exception_save,
	.restore	= pmr_exception_restore,
};

struct cpu_exception_mask_handler *cpu_exception = &daif_handler;

int set_exception_mask_handler(int type)
{
	switch (type) {
	case 0:
		cpu_exception = &daif_handler;
		break;
	case 1:
		cpu_exception = &pmr_handler;
		break;
	/* case 2: reserved for FEAT_NMI */
	default:
		return -EINVAL;
	}
	pr_info("Exception mask handlers: %ps %ps %ps\n",
					cpu_exception->mask,
					cpu_exception->save,
					cpu_exception->restore);
	return 0;
}
