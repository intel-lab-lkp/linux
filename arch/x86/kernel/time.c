// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (c) 1991,1992,1995  Linus Torvalds
 *  Copyright (c) 1994  Alan Modra
 *  Copyright (c) 1995  Markus Kuhn
 *  Copyright (c) 1996  Ingo Molnar
 *  Copyright (c) 1998  Andrea Arcangeli
 *  Copyright (c) 2002,2006  Vojtech Pavlik
 *  Copyright (c) 2003  Andi Kleen
 *
 */

#include <linux/acpi.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i8253.h>
#include <linux/time.h>

#include <asm/vsyscall.h>
#include <asm/x86_init.h>
#include <asm/i8259.h>
#include <asm/timer.h>
#include <asm/hpet.h>
#include <asm/time.h>

unsigned long profile_pc(struct pt_regs *regs)
{
	return instruction_pointer(regs);
}
EXPORT_SYMBOL(profile_pc);

/*
 * Default timer interrupt handler for PIT/HPET
 */
static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	global_clock_event->event_handler(global_clock_event);
	return IRQ_HANDLED;
}

static void __init setup_default_timer_irq(void)
{
	unsigned long flags = IRQF_NOBALANCING | IRQF_IRQPOLL | IRQF_TIMER;

	/*
	 * Unconditionally register the legacy timer interrupt; even
	 * without legacy PIC/PIT we need this for the HPET0 in legacy
	 * replacement mode.
	 */
	if (request_irq(0, timer_interrupt, flags, "timer", NULL))
		pr_info("Failed to register legacy timer interrupt\n");
}

/* Default timer init function */
void __init hpet_time_init(void)
{
	if (!hpet_enable()) {
		if (!pit_timer_init())
			return;
	}

	setup_default_timer_irq();
}

static __init void x86_late_time_init(void)
{
	/*
	 * Before PIT/HPET init, select the interrupt mode. This is required
	 * to make the decision whether PIT should be initialized correct.
	 */
	x86_init.irqs.intr_mode_select();

	/*
	 * ACPI hardware-reduced mode unconditionally stubs .timer_init() to
	 * a noop, on the assumption that no legacy timer exists. Some such
	 * platforms do provide a real HPET, so let "hpet=force" give it a
	 * chance to come up. Only hpet_enable() is wanted here, not
	 * hpet_time_init(): its PIT fallback and legacy timer interrupt have
	 * no hardware behind them on such platforms, and registering a PIT
	 * clockevent whose interrupt can never be delivered also fails the
	 * local APIC timer's calibration cross-check, disabling that too.
	 *
	 * This also covers hardware-reduced platforms that install their own
	 * .timer_init() rather than being stubbed (e.g. Intel MID, which
	 * sets up the local APIC timer only). None of them enable the HPET
	 * themselves, so it cannot end up registered twice.
	 */
	if (IS_ENABLED(CONFIG_HPET_TIMER) && acpi_reduced_hardware() &&
	    hpet_force_user)
		hpet_enable();

	/* Setup the legacy timers */
	x86_init.timers.timer_init();

	/*
	 * After PIT/HPET timers init, set up the final interrupt mode for
	 * delivering IRQs.
	 */
	x86_init.irqs.intr_mode_init();
	tsc_init();

	if (cpu_feature_enabled(X86_FEATURE_WAITPKG))
		use_tpause_delay();
}

/*
 * Initialize TSC and delay the periodic timer init to
 * late x86_late_time_init() so ioremap works.
 */
void __init time_init(void)
{
	late_time_init = x86_late_time_init;
}

/*
 * Sanity check the vdso related archdata content.
 */
void clocksource_arch_init(struct clocksource *cs)
{
	if (cs->vdso_clock_mode == VDSO_CLOCKMODE_NONE)
		return;

	if (cs->mask != CLOCKSOURCE_MASK(64)) {
		pr_warn("clocksource %s registered with invalid mask %016llx for VDSO. Disabling VDSO support.\n",
			cs->name, cs->mask);
		cs->vdso_clock_mode = VDSO_CLOCKMODE_NONE;
	}
}
