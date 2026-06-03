// SPDX-License-Identifier: GPL-2.0
/*
 * arm64 SDEI-based cross-CPU NMI service.
 *
 * Delivering an "NMI-shaped" event to an EL1 context that has locally
 * masked interrupts, on silicon without FEAT_NMI, can be done two ways:
 *
 *   - pseudo-NMI: mask "interrupts" via the GIC priority register
 *     (ICC_PMR_EL1) instead of PSTATE.DAIF, leaving a high-priority band
 *     deliverable. Functionally this works -- but it reimplements every
 *     local_irq_disable()/enable() and exception entry/exit as a PMR
 *     write plus synchronisation, a cost paid on that hot path forever,
 *     whether or not an NMI is ever delivered.
 *
 *   - SDEI: leave interrupt masking as the cheap PSTATE.DAIF operation
 *     and have the firmware bounce an EL3-routed Group-0 SGI back to
 *     NS-EL1 as an event callback. The cost is a firmware round-trip,
 *     but only at the rare moment delivery is actually needed.
 *
 * This driver takes the second path: it keeps the IRQ-mask hot path
 * free and pays only when it fires, which is what makes cross-CPU NMI
 * affordable on hardware where the pseudo-NMI tax isn't, until FEAT_NMI
 * makes NMI masking cheap in the architecture itself.
 *
 * Capabilities provided:
 *
 *   - sdei_nmi_trigger_cpumask_backtrace() — override for arm64's
 *     arch_trigger_cpumask_backtrace(), so sysrq-l, RCU stall dumps,
 *     hardlockup_all_cpu_backtrace, soft-lockup/hung-task secondary
 *     dumps all reach interrupt-masked CPUs.
 *
 *   - the hardlockup-detector backend (watchdog_hardlockup_enable/
 *     disable/probe()), when CONFIG_HARDLOCKUP_DETECTOR is also on.
 *     ARM_SDEI_NMI selects HAVE_HARDLOCKUP_DETECTOR_ARCH, so the
 *     framework picks this backend. The detection source is chosen at
 *     boot: SDEI when the firmware has it, otherwise a perf-PMU NMI
 *     counter if one is available (pseudo-NMI enabled). One kernel image
 *     thus serves SDEI and non-SDEI hosts.
 *
 * Delivery uses the standard SDEI software-signalled event (event 0) and
 * SDEI_EVENT_SIGNAL. We register a handler for event 0, enable it, and
 * poke a target CPU with sdei_event_signal(0, mpidr): firmware makes
 * event 0 pending on that PE and dispatches the handler NMI-like,
 * regardless of the target's DAIF.
 * Availability is simply whether event 0 registers and enables -- if SDEI
 * and its software-signalled event are present we use it, otherwise the
 * driver stays inert.
 */

#define pr_fmt(fmt) "sdei_nmi: " fmt

#include <linux/arm_sdei.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/nmi.h>
#include <linux/percpu-defs.h>
#include <linux/perf_event.h>
#include <linux/perf/arm_pmu.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/sched/clock.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/nmi.h>
#include <asm/smp_plat.h>

static bool sdei_nmi_available;

#define SDEI_NMI_EVENT			0

static int sdei_nmi_handler(u32 event, struct pt_regs *regs, void *arg)
{
	/*
	 * Both consumers no-op on a CPU that wasn't actually requested:
	 * nmi_cpu_backtrace() unless this CPU's bit is set in the global
	 * backtrace mask, and watchdog_hardlockup_check() unless this CPU's
	 * hrtimer_interrupts counter has stalled. The latter is only
	 * declared when the watchdog backend is built in (COUNTS_HRTIMER,
	 * pulled by ARM_SDEI_NMI when HARDLOCKUP_DETECTOR is enabled).
	 */
	nmi_cpu_backtrace(regs);
#ifdef CONFIG_HARDLOCKUP_DETECTOR_COUNTS_HRTIMER
	watchdog_hardlockup_check(smp_processor_id(), regs);
#endif
	return SDEI_EV_HANDLED;
}

static void sdei_nmi_fire(unsigned int target_cpu)
{
	int err = sdei_event_signal(SDEI_NMI_EVENT, cpu_logical_map(target_cpu));

	if (err)
		pr_warn("SDEI_EVENT_SIGNAL to CPU %u failed: %d\n",
			target_cpu, err);
}

/*
 * Raise callback for nmi_trigger_cpumask_backtrace(): signal event 0
 * at every CPU still pending in @mask. The framework excludes the local
 * CPU from @mask before calling us.
 */
static void sdei_nmi_raise_backtrace(cpumask_t *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		sdei_nmi_fire(cpu);
}

/*
 * Override hook for arch_trigger_cpumask_backtrace() (see
 * arch/arm64/kernel/smp.c). Returns true when SDEI handled the request,
 * which is the case whenever SDEI is active; on a false return the arch
 * falls back to its regular-IRQ (or pseudo-NMI, if enabled) IPI.
 *
 * On a kernel built without paying the pseudo-NMI hot-path cost (the
 * usual case for this driver's target), the IPI can't reach a CPU that
 * has interrupts masked -- so the backtrace of the one CPU you care
 * about comes back empty. SDEI is dispatched out of EL3 and lands
 * regardless of the target's DAIF, without taxing the IRQ-mask path.
 */
bool sdei_nmi_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu)
{
	if (!sdei_nmi_available)
		return false;

	nmi_trigger_cpumask_backtrace(mask, exclude_cpu,
				      sdei_nmi_raise_backtrace);
	return true;
}

#ifdef CONFIG_HARDLOCKUP_DETECTOR_COUNTS_HRTIMER

/*
 * SDEI watchdog source: a per-CPU hrtimer pets its own heartbeat and
 * checks its buddy's; on a stall it signals event 0 at the buddy,
 * whose SDEI handler then runs watchdog_hardlockup_check().
 */
#define SDEI_NMI_WATCHDOG_TICK_MS	1000

static cpumask_t __read_mostly sdei_nmi_watchdog_cpus;
static DEFINE_PER_CPU(struct hrtimer, sdei_nmi_watchdog_hrtimer);
static DEFINE_PER_CPU(u64, sdei_nmi_watchdog_heartbeat_ns);

static unsigned int sdei_nmi_watchdog_next_cpu(unsigned int cpu)
{
	unsigned int next = cpumask_next_wrap(cpu, &sdei_nmi_watchdog_cpus);

	if (next == cpu)
		return nr_cpu_ids;
	return next;
}

static enum hrtimer_restart sdei_nmi_watchdog_hrtimer_fn(struct hrtimer *t)
{
	unsigned int this_cpu = smp_processor_id();
	unsigned int buddy;
	u64 now = local_clock();
	u64 buddy_hb, thresh_ns;

	this_cpu_write(sdei_nmi_watchdog_heartbeat_ns, now);

	buddy = sdei_nmi_watchdog_next_cpu(this_cpu);
	if (buddy >= nr_cpu_ids)
		goto restart;

	/* pair with smp_wmb() in start_watchdog/stop_watchdog */
	smp_rmb();

	buddy_hb = per_cpu(sdei_nmi_watchdog_heartbeat_ns, buddy);
	thresh_ns = (u64)watchdog_thresh * NSEC_PER_SEC;

	if (now > buddy_hb + thresh_ns) {
		/*
		 * Fire every tick while the buddy looks stale: the framework's
		 * watchdog_hardlockup_check() needs two consecutive calls
		 * before it'll declare a lockup (first call updates
		 * hrtimer_interrupts_saved; second confirms the counter
		 * hasn't moved). One-shot firing wedges the detection at
		 * step 1. The cost of an extra SMC per second on a truly
		 * wedged CPU is negligible; the alternative is silent
		 * non-detection.
		 */
		pr_warn_ratelimited("watchdog: CPU %u no heartbeat for %llu ms (thresh %us), firing NMI from CPU %u\n",
				    buddy,
				    (now - buddy_hb) / NSEC_PER_MSEC,
				    watchdog_thresh, this_cpu);
		sdei_nmi_fire(buddy);
	}

restart:
	hrtimer_forward_now(t, ms_to_ktime(SDEI_NMI_WATCHDOG_TICK_MS));
	return HRTIMER_RESTART;
}

static void sdei_nmi_watchdog_enable(unsigned int cpu)
{
	struct hrtimer *t = this_cpu_ptr(&sdei_nmi_watchdog_hrtimer);

	if (cpumask_test_cpu(cpu, &sdei_nmi_watchdog_cpus))
		return;

	this_cpu_write(sdei_nmi_watchdog_heartbeat_ns, local_clock());

	hrtimer_setup(t, sdei_nmi_watchdog_hrtimer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED);

	/* pair with smp_rmb() in the hrtimer callback */
	smp_wmb();
	cpumask_set_cpu(cpu, &sdei_nmi_watchdog_cpus);

	hrtimer_start(t, ms_to_ktime(SDEI_NMI_WATCHDOG_TICK_MS),
		      HRTIMER_MODE_REL_PINNED);
}

static void sdei_nmi_watchdog_disable(unsigned int cpu)
{
	if (!cpumask_test_cpu(cpu, &sdei_nmi_watchdog_cpus))
		return;

	cpumask_clear_cpu(cpu, &sdei_nmi_watchdog_cpus);
	/* pair with smp_rmb() in the hrtimer callback */
	smp_wmb();

	hrtimer_cancel(this_cpu_ptr(&sdei_nmi_watchdog_hrtimer));
}

/*
 * Perf-NMI fallback source, used when SDEI is absent but the PMU IRQ is
 * a (pseudo-)NMI. A per-CPU cycle counter overflows into the same
 * watchdog_hardlockup_check(). This is the stock arm64 perf hardlockup
 * detector, minimal-copied here because the framework's
 * HARDLOCKUP_DETECTOR_PERF is compile-excluded once we select
 * HAVE_HARDLOCKUP_DETECTOR_ARCH (it would otherwise provide a second
 * definition of these same hooks).
 */
static struct perf_event_attr perf_wd_attr = {
	.type		= PERF_TYPE_HARDWARE,
	.config		= PERF_COUNT_HW_CPU_CYCLES,
	.size		= sizeof(struct perf_event_attr),
	.pinned		= 1,
	.disabled	= 1,
};

static DEFINE_PER_CPU(struct perf_event *, perf_wd_event);

static u64 perf_wd_period(int cpu)
{
	/* 5 GHz safe max when cpufreq is unavailable, as in watchdog_hld.c. */
	u64 hz = cpufreq_get_hw_max_freq(cpu) * 1000UL;

	return (hz ? hz : 5000000000UL) * watchdog_thresh;
}

static void perf_wd_overflow(struct perf_event *event,
			     struct perf_sample_data *data,
			     struct pt_regs *regs)
{
	watchdog_hardlockup_check(smp_processor_id(), regs);
}

static void perf_wd_enable(unsigned int cpu)
{
	struct perf_event *evt;

	if (this_cpu_read(perf_wd_event))
		return;

	perf_wd_attr.sample_period = perf_wd_period(cpu);
	evt = perf_event_create_kernel_counter(&perf_wd_attr, cpu, NULL,
					       perf_wd_overflow, NULL);
	if (IS_ERR(evt)) {
		pr_warn_once("perf event create on CPU %u failed: %ld\n",
			     cpu, PTR_ERR(evt));
		return;
	}

	this_cpu_write(perf_wd_event, evt);
	perf_event_enable(evt);
}

static void perf_wd_disable(unsigned int cpu)
{
	struct perf_event *evt = this_cpu_read(perf_wd_event);

	if (!evt)
		return;

	perf_event_disable(evt);
	perf_event_release_kernel(evt);
	this_cpu_write(perf_wd_event, NULL);
}

/* Set by the late_initcall below once the perf fallback is chosen. */
static bool perf_wd_active;

void watchdog_hardlockup_enable(unsigned int cpu)
{
	WARN_ON_ONCE(cpu != smp_processor_id());

	if (sdei_nmi_available)
		sdei_nmi_watchdog_enable(cpu);
	else if (perf_wd_active)
		perf_wd_enable(cpu);
}

void watchdog_hardlockup_disable(unsigned int cpu)
{
	WARN_ON_ONCE(cpu != smp_processor_id());

	if (sdei_nmi_available)
		sdei_nmi_watchdog_disable(cpu);
	else if (perf_wd_active)
		perf_wd_disable(cpu);
}

int __init watchdog_hardlockup_probe(void)
{
	return (sdei_nmi_available || perf_wd_active) ? 0 : -ENODEV;
}

/*
 * Phase 2 of init, at late_initcall so it runs after both our own
 * device_initcall (SDEI decision) and armv8_pmuv3's (which is what makes
 * arm_pmu_irq_is_nmi() read true). If SDEI didn't claim the watchdog and
 * the PMU IRQ is a (pseudo-)NMI, take the perf fallback. Deciding here,
 * after both device_initcalls, keeps the choice deterministic -- no race
 * over which initcall ran first, and no flip from perf to SDEI.
 */
static int __init perf_wd_init(void)
{
	if (sdei_nmi_available)
		return 0;	/* SDEI already owns the watchdog */

	if (IS_ENABLED(CONFIG_ARM64_PSEUDO_NMI) && arm_pmu_irq_is_nmi()) {
		perf_wd_active = true;
		pr_info("no SDEI firmware; using perf-NMI watchdog fallback\n");
		lockup_detector_retry_init();
	}
	return 0;
}
late_initcall(perf_wd_init);

#endif /* CONFIG_HARDLOCKUP_DETECTOR_COUNTS_HRTIMER */

/*
 * device_initcall (after arch_initcall(sdei_init), so the SDEI subsystem
 * is up): probe the firmware, register the event, and turn on the
 * cross-CPU service. If the probe fails the driver stays inert and the
 * override hooks decline, leaving the arch's own paths in place.
 */
static int __init sdei_nmi_init(void)
{
	int err;

	err = sdei_event_register(SDEI_NMI_EVENT, sdei_nmi_handler, NULL);
	if (err) {
		pr_err("sdei_event_register(%u) failed: %d\n",
		       SDEI_NMI_EVENT, err);
		return 0;
	}

	err = sdei_event_enable(SDEI_NMI_EVENT);
	if (err) {
		pr_err("sdei_event_enable(%u) failed: %d\n",
		       SDEI_NMI_EVENT, err);
		sdei_event_unregister(SDEI_NMI_EVENT);
		return 0;
	}

	sdei_nmi_available = true;
	pr_info("using SDEI cross-CPU NMI (SDEI_EVENT_SIGNAL, event %u)\n",
		SDEI_NMI_EVENT);

	/*
	 * lockup_detector_init() ran in early init and found no hardlockup
	 * backend yet; re-probe now that SDEI owns the watchdog.
	 */
	if (IS_ENABLED(CONFIG_HARDLOCKUP_DETECTOR_COUNTS_HRTIMER))
		lockup_detector_retry_init();

	return 0;
}
device_initcall(sdei_nmi_init);
