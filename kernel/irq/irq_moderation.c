// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

/*
 * Copyright (C) 2025-2026 Google LLC
 *
 * Global Software Interrupt Moderation (GSIM) core logic.
 */

#include <linux/cpuhotplug.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/suspend.h>

#include "internals.h"
#include "irq_moderation.h"

/*
 * Global Software Interrupt Moderation (GSIM)
 *
 * Some platforms show reduced I/O performance when the total device interrupt
 * rate across the entire platform becomes too high. To address the problem,
 * GSIM runs after the handler to implement software interrupt moderation
 * with programmable delay.
 *
 * === ARCHITECTURE ===
 *
 * INTERRUPT HANDLING (for interrupt types that support moderation)
 * - irq_start_moderation() runs under desc->lock right after the interrupt handler.
 *   If the interrupt must be moderated, sets IRQD_IRQ_INPROGRESS and IRQD_MODERATED,
 *   calls __disable_irq(), adds the irq_desc to a per-CPU list of moderated interrupts,
 *   and starts a moderation timer if not yet active;
 * - handle_xx_irq() is modified so that when called on a moderated irq_desc it
 *   calls mask_irq(), sets IRQS_PENDING and returns immediately;
 * - the timer callback drains the moderation list: on each irq_desc it acquires
 *   desc->lock, and if desc->action != NULL calls __enable_irq(), possibly calling
 *   the handler if IRQS_PENDING is set.
 *
 * INTERRUPT TEARDOWN
 * It is protected by IRQD_IRQ_INPROGRESS and checking desc->action != NULL.
 * This works because free_irq() runs in two steps:
 * - first clear desc->action (under lock),
 * - then call synchronize_irq(), which blocks on IRQD_IRQ_INPROGRESS
 *   before freeing resources.
 * When the moderation timer races with free_irq() we can have two cases:
 * 1. timer runs before clearing desc->action. In this case __enable_irq()
 *    is valid and the subsequent free_irq() will complete as intended
 * 2. desc->action is cleared before the timer runs. In this case synchronize_irq()
 *    will block until the timer expires (remember moderation delays are very short,
 *    comparable to C-state exit times), __enable_irq() will not be run,
 *    and free_irq() will complete successfully.
 *
 * INTERRUPT MIGRATION
 * It is protected by IRQD_IRQ_INPROGRESS that prevents running the handler on the
 * new CPU while an interrupt is moderated.
 *
 * HOTPLUG
 * During CPU shutdown, the kernel moves timers and reassigns interrupt affinity
 * to a new CPU. The easiest way and most robust way to guarantee that pending
 * events are handled correctly is to use a per-CPU "moderation_allowed" flag
 * and hotplug callbacks on CPUHP_AP_ONLINE_DYN (some others are equally good):
 * - on setup, set the flag. That will allow interrupts to be moderated.
 * - on shutdown, with interrupts disabled, 1. clear the flag thus preventing
 *   more interrupts to be moderated on that CPU, 2. flush the list of moderated
 *   interrupts (as if the timer had fired), and 3. cancel the timer.
 * This avoids depending with the internals of the up/down sequence.
 *
 * STATIC ENABLING
 * GSIM is disabled by default (delay_ns = 0). To statically enable it:
 * 1. Initialize irq_mod_params.delay_ns to a non-zero value (e.g., 100000 for 100us)
 *    in kernel/irq/irq_moderation.c.
 * 2. Call irq_settings_set_moderatable(desc) during interrupt allocation
 *    (e.g., in __setup_irq() in kernel/irq/manage.c) for the desired interrupts.
 *
 * SUSPEND & HIBERNATION
 * During Suspend-to-RAM or Suspend-to-Disk (Hibernation), secondary CPUs are
 * taken offline, which triggers the CPU hotplug teardown and setup callbacks.
 * However, the boot processor is never taken offline via hotplug.
 *
 * To ensure the boot processor's GSIM state is safely drained/disabled before
 * suspend, and safely re-enabled after resume/restore, we register a PM notifier.
 *
 * The PM notifier handles all suspend, hibernation, and restore transitions:
 * - On prepare (*_PREPARE): runs mod_pm_prepare_cb() on all CPUs (via IPI)
 *   to clear the allowed flag and drain pending moderated interrupts, and
 *   then cancels GSIM timers on all online CPUs outside IPI context.
 * - On resume/restore (POST_*): runs mod_pm_resume_cb() on all CPUs to
 *   safely re-allow moderation. We do NOT run cpu_setup_cb() here to avoid
 *   dangerous double-initialization of active hrtimers or list heads on
 *   already-online secondary CPUs.
 */

/*
 * GSIM parameters. Initialize delay_ns here to statically enable moderation
 * (e.g. .delay_ns = 100000).
 */
struct irq_mod_params irq_mod_params ____cacheline_aligned;

DEFINE_PER_CPU_ALIGNED(struct irq_mod_state, irq_mod_state);

DEFINE_STATIC_KEY_FALSE(irq_moderation_enabled_key);

static void update_enable_key(void)
{
	if (irq_mod_params.delay_ns != 0)
		static_branch_enable(&irq_moderation_enabled_key);
	else
		static_branch_disable(&irq_moderation_enabled_key);
}

/* Actually start moderation. */
bool irq_moderation_do_start(struct irq_desc *desc, struct irq_mod_state *m)
{
	lockdep_assert_held(&desc->lock);

	if (!hrtimer_is_queued(&m->timer)) {
		const unsigned int min_delay_ns = 10000;
		const u64 slack_ns = 2000;

		/* Accumulate sleep time, no moderation if too small. */
		m->sleep_ns += READ_ONCE(irq_mod_params.delay_ns);
		if (m->sleep_ns < min_delay_ns)
			return false;
		/* We need moderation, start the timer. */
		m->timer_set++;
		hrtimer_start_range_ns(&m->timer, ns_to_ktime(m->sleep_ns),
				       slack_ns, HRTIMER_MODE_REL_PINNED_HARD);
	}

	/*
	 * Add to the timer list, set appropriate flags, and call
	 * __disable_irq() to prevent serving subsequent interrupts.
	 */
	m->enqueue++;
	list_add(&desc->swmod_state.swmod_node, &m->descs);
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_MODERATED);
	__disable_irq(desc);
	return true;
}

static void clean_moderation_state(struct irq_desc *desc)
{
	/*
	 * Clearing IRQD_IRQ_INPROGRESS allows synchronize_irq() to complete,
	 * signaling that GSIM teardown for this descriptor is finished.
	 */
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS | IRQD_MODERATED);
	/* Only enable if action is set, protect against concurrent free_irq(). */
	if (desc->action)
		__enable_irq(desc);
}

/* Used on timer expiration or CPU shutdown. */
static void drain_desc_list(struct irq_mod_state *m)
{
	struct irq_desc *desc, *next;

	/* Remove from list and enable interrupts back. */
	list_for_each_entry_safe(desc, next, &m->descs, swmod_state.swmod_node) {
		guard(raw_spinlock)(&desc->lock);
		list_del_init(&desc->swmod_state.swmod_node);
		clean_moderation_state(desc);
	}
}

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	lockdep_assert_irqs_disabled();

	drain_desc_list(m);
	/* Prepare to accumulate next moderation delay. */
	m->sleep_ns = 0;
	return HRTIMER_NORESTART;
}

/* Hotplug callback for setup. */
static int cpu_setup_cb(unsigned int cpu)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	hrtimer_setup(&m->timer, timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	INIT_LIST_HEAD(&m->descs);
	/* Ensure initialization is visible before setting the flag. */
	smp_store_release(&m->initialized, true);
	m->moderation_allowed = true;
	return 0;
}

/*
 * Hotplug callback for shutdown.
 * Mark the CPU as offline for moderation, and drain the list of masked
 * interrupts. Any subsequent interrupt on this CPU will not be
 * moderated, but they will be on the new target.
 */
static int cpu_remove_cb(unsigned int cpu)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/* Protect the desc list, interrupts could modify it. */
	scoped_guard(irqsave) {
		m->moderation_allowed = false;
		drain_desc_list(m);
	}
	/* Run hrtimer_cancel() outside hardirq/IPI context. */
	hrtimer_cancel(&m->timer);
	/* Ensure state is visible before clearing the flag. */
	smp_store_release(&m->initialized, false);
	return 0;
}

static void mod_pm_prepare_cb(void *arg)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/* Called via IPI so local interrupts are disabled. */
	if (mod_state_initialized(m)) {
		m->moderation_allowed = false;
		drain_desc_list(m);
	}
}

static void mod_pm_resume_cb(void *arg)
{
	struct irq_mod_state *m = this_cpu_ptr(&irq_mod_state);

	/*
	 * The hrtimer and list head are already initialized (either at boot
	 * or during hotplug CPU online). We must not re-initialize them here
	 * as they might already be active if devices resumed and fired
	 * interrupts before this notifier ran.
	 */
	if (mod_state_initialized(m))
		m->moderation_allowed = true;
}

static int mod_pm_notifier_cb(struct notifier_block *nb, unsigned long event, void *unused)
{
	int cpu;

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		on_each_cpu(mod_pm_prepare_cb, NULL, 1);
		/* Run hrtimer_cancel() outside hardirq/IPI context. */
		for_each_online_cpu(cpu) {
			struct irq_mod_state *m = per_cpu_ptr(&irq_mod_state, cpu);

			if (mod_state_initialized(m))
				hrtimer_cancel(&m->timer);
		}
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		on_each_cpu(mod_pm_resume_cb, NULL, 1);
		break;
	}
	return NOTIFY_OK;
}

struct notifier_block mod_nb = {
	.notifier_call	= mod_pm_notifier_cb,
	.priority	= 100,
};

static int __init init_irq_moderation(void)
{
	int cpuhp_state;
	int ret;

	cpuhp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "sw_moderation",
					cpu_setup_cb, cpu_remove_cb);
	if (cpuhp_state < 0) {
		pr_err("%s: Failed to setup hotplug notifier\n", __func__);
		return cpuhp_state;
	}

	ret = register_pm_notifier(&mod_nb);
	if (ret < 0) {
		pr_err("%s: Failed to register pm notifier\n", __func__);
		goto cleanup;
	}

	/* Enable if the defaults require it. */
	update_enable_key();
	return 0;

cleanup:
	cpuhp_remove_state(cpuhp_state);
	return ret;
}
device_initcall(init_irq_moderation);
