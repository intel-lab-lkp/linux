// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpuhotplug.h>
#include <linux/ftrace.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched/clock.h>
#include <linux/irqflags.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/trace_seq.h>
#include <linux/workqueue.h>

#include "kwatch.h"

/* Minimum spacing between cross-CPU arm broadcasts, per CPU. */
#define KWATCH_ARM_IPI_MIN_INTERVAL_NS	1000000ULL

static LIST_HEAD(kwatch_all_wp_list);
static struct kwatch_watchpoint **kwatch_wp_slots;
static u16 kwatch_wp_nr;
static DEFINE_MUTEX(kwatch_all_wp_mutex);
static unsigned long kwatch_dummy_holder __aligned(8);
static int kwatch_hwbp_cpuhp_state = CPUHP_INVALID;
static atomic_long_t kwatch_arm_ipi_suppressed;

unsigned long kwatch_hwbp_arm_ipi_suppressed(void)
{
	return atomic_long_read(&kwatch_arm_ipi_suppressed);
}

#define CREATE_TRACE_POINTS
#include <trace/events/kwatch.h>

/*
 * Render the saved stack like the ftrace built-in stacktrace / dump_stack()
 * style. Symbol resolution runs at trace read time, not in the hit path.
 */
const char *kwatch_trace_print_stack(struct trace_seq *p,
				     const unsigned long *stack,
				     unsigned int nr)
{
	const char *ret = trace_seq_buffer_ptr(p);
	unsigned int i;

	for (i = 0; i < nr; i++)
		trace_seq_printf(p, "\n => %pS", (void *)stack[i]);
	trace_seq_putc(p, 0);
	return ret;
}

static void kwatch_hwbp_handler(struct perf_event *bp,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	struct kwatch_watchpoint *wp = bp->overflow_handler_context;
	unsigned long stack_entries[KWATCH_STACK_DEPTH];
	unsigned int stack_nr;

	if (!kwatch_probe_validate_hit(regs, wp->arm_tsk))
		return;

	stack_nr = stack_trace_save_regs(regs, stack_entries, KWATCH_STACK_DEPTH, 2);
	trace_kwatch_hit(instruction_pointer(regs), kernel_stack_pointer(regs),
			 bp->attr.bp_addr, local_clock(),
			 stack_entries, stack_nr);
}

static void kwatch_hwbp_arm_local(void *info)
{
	struct kwatch_watchpoint *wp = info;
	struct perf_event *bp;
	unsigned long flags;
	int cpu, err;

	local_irq_save(flags);

	cpu = smp_processor_id();
	bp = per_cpu(*wp->event, cpu);

	if (unlikely(!bp))
		goto out;

	kwatch_probe_mute(true);
	barrier();

	/*
	 * On success this also updates the per-CPU bp->attr, so the hit
	 * handler reports what THIS CPU is watching instead of the shared
	 * wp->attr, which another CPU may be re-pointing.
	 */
	err = modify_wide_hw_breakpoint_local(bp, &wp->attr);
	if (unlikely(err))
		WARN_ONCE(1,
			  "KWatch: HWBP reinstall failed on CPU%d (err=%d, addr=0x%llx, len=%llu)\n",
			  cpu, err, wp->attr.bp_addr, wp->attr.bp_len);

	barrier();
	kwatch_probe_mute(false);

out:
	local_irq_restore(flags);
}

static inline void kwatch_hwbp_try_recycle(struct kwatch_watchpoint *wp)
{
	if (atomic_dec_and_test(&wp->pending_ipis)) {
		if (!READ_ONCE(wp->teardown))
			atomic_set_release(&wp->in_use, 0);

		atomic_dec(&wp->refcount);
	}
}

static void kwatch_hwbp_disarm_local(void *info)
{
	struct kwatch_watchpoint *wp = info;

	kwatch_hwbp_arm_local(info);
	kwatch_hwbp_try_recycle(wp);
}

static int kwatch_hwbp_cpu_online(unsigned int cpu)
{
	struct perf_event_attr attr;
	struct kwatch_watchpoint *wp;
	struct perf_event *bp;

	mutex_lock(&kwatch_all_wp_mutex);
	list_for_each_entry(wp, &kwatch_all_wp_list, list) {
		attr = wp->attr;
		attr.bp_addr = (unsigned long)&kwatch_dummy_holder;
		bp = perf_event_create_kernel_counter(&attr, cpu, NULL,
						      kwatch_hwbp_handler, wp);
		if (IS_ERR(bp)) {
			pr_warn("%s failed to create watch on CPU %d: %ld\n",
				__func__, cpu, PTR_ERR(bp));
			continue;
		}
		per_cpu(*wp->event, cpu) = bp;
	}
	mutex_unlock(&kwatch_all_wp_mutex);
	return 0;
}

static int kwatch_hwbp_cpu_offline(unsigned int cpu)
{
	struct kwatch_watchpoint *wp;
	struct perf_event *bp;

	mutex_lock(&kwatch_all_wp_mutex);
	list_for_each_entry(wp, &kwatch_all_wp_list, list) {
		bp = per_cpu(*wp->event, cpu);
		if (bp) {
			unregister_hw_breakpoint(bp);
			per_cpu(*wp->event, cpu) = NULL;
		}
	}
	mutex_unlock(&kwatch_all_wp_mutex);
	return 0;
}

int kwatch_hwbp_get(struct kwatch_watchpoint **out_wp)
{
	struct kwatch_watchpoint *wp;
	int i;

	/*
	 * Per-slot cmpxchg claim: safe for concurrent consumers on any CPU,
	 * unlike llist_del_first() which requires a single consumer.
	 */
	for (i = 0; i < kwatch_wp_nr; i++) {
		wp = kwatch_wp_slots[i];
		if (atomic_read(&wp->in_use))
			continue;
		if (atomic_cmpxchg(&wp->in_use, 0, 1) == 0) {
			atomic_inc(&wp->refcount);
			*out_wp = wp;
			return 0;
		}
	}
	return -EBUSY;
}

void kwatch_hwbp_arm(struct kwatch_watchpoint *wp, unsigned long addr, u16 len)
{
	static DEFINE_PER_CPU(u64, last_ipi_time);
	int cur_cpu;
	call_single_data_t *csd;
	int cpu;
	bool is_disarm = (addr == (unsigned long)&kwatch_dummy_holder);
	bool skip_remote = false;

	wp->attr.bp_addr = addr;
	wp->attr.bp_len = len;

	if (!is_disarm)
		wp->arm_tsk = current;

	/* ensure attr update visible to other cpu before sending IPI */
	smp_wmb();

	atomic_set(&wp->pending_ipis, 1);
	cur_cpu = get_cpu();

	/*
	 * Rate-limit only the cross-CPU broadcast, never the local re-point.
	 * Arming the current CPU is free and must always reflect this window;
	 * only the remote IPI fan-out is throttled to keep a hot function from
	 * storming every CPU. A suppressed broadcast means remote CPUs keep
	 * watching the previous address for that window (a missed remote-CPU
	 * writer is possible) - hence the visible counter, and why kwatch
	 * targets low-frequency functions. Disarm is never throttled: the
	 * slot must always be released.
	 */
	if (!is_disarm) {
		u64 now = local_clock();
		u64 last = this_cpu_read(last_ipi_time);

		if (now - last < KWATCH_ARM_IPI_MIN_INTERVAL_NS) {
			atomic_long_inc(&kwatch_arm_ipi_suppressed);
			skip_remote = true;
		} else {
			this_cpu_write(last_ipi_time, now);
		}
	}

	if (!skip_remote) {
		for_each_online_cpu(cpu) {
			if (cpu == cur_cpu)
				continue;

			if (is_disarm)
				atomic_inc(&wp->pending_ipis);

			csd = per_cpu_ptr(is_disarm ? wp->csd_disarm : wp->csd_arm,
					  cpu);
			/*
			 * The arm path ignores a -EBUSY return: a wp has a single
			 * owner (claimed via kwatch_hwbp_get(), held until exit)
			 * and is armed once per window, and the per-CPU csd queue
			 * is FIFO, so this window's csd_arm cannot still be pending
			 * from a prior window (its disarm, queued later, gates the
			 * wp's reuse). Do not "fix" this into a retry.
			 */
			if (smp_call_function_single_async(cpu, csd) && is_disarm)
				kwatch_hwbp_try_recycle(wp);
		}
	}
	put_cpu();

	if (is_disarm)
		kwatch_hwbp_disarm_local(wp);
	else
		kwatch_hwbp_arm_local(wp);
}

int kwatch_hwbp_put(struct kwatch_watchpoint *wp)
{
	kwatch_hwbp_arm(wp, (unsigned long)&kwatch_dummy_holder,
			sizeof(unsigned long));

	return 0;
}

void kwatch_hwbp_free(void)
{
	struct kwatch_watchpoint *wp, *tmp;

	kwatch_wp_nr = 0;
	kfree(kwatch_wp_slots);
	kwatch_wp_slots = NULL;

	if (kwatch_hwbp_cpuhp_state != CPUHP_INVALID) {
		cpuhp_remove_state_nocalls(kwatch_hwbp_cpuhp_state);
		kwatch_hwbp_cpuhp_state = CPUHP_INVALID;
	}

	mutex_lock(&kwatch_all_wp_mutex);
	list_for_each_entry_safe(wp, tmp, &kwatch_all_wp_list, list) {
		list_del(&wp->list);

		WRITE_ONCE(wp->teardown, true);
		atomic_dec(&wp->refcount);

		/* Wait for all async IPIs to finish */
		while (atomic_read(&wp->refcount) > 0)
			cpu_relax();

		unregister_wide_hw_breakpoint(wp->event);
		free_percpu(wp->csd_arm);
		free_percpu(wp->csd_disarm);
		kfree(wp);
	}
	mutex_unlock(&kwatch_all_wp_mutex);
}

int kwatch_hwbp_prealloc(u16 max_watch)
{
	struct kwatch_watchpoint *wp;
	int success = 0, cpu;
	int ret;

	atomic_long_set(&kwatch_arm_ipi_suppressed, 0);

	while (!max_watch || success < max_watch) {
		wp = kzalloc_obj(*wp);
		if (!wp)
			break;

		wp->csd_arm = alloc_percpu(call_single_data_t);
		wp->csd_disarm = alloc_percpu(call_single_data_t);
		if (!wp->csd_arm || !wp->csd_disarm) {
			free_percpu(wp->csd_arm);
			free_percpu(wp->csd_disarm);
			kfree(wp);
			break;
		}

		for_each_possible_cpu(cpu) {
			INIT_CSD(per_cpu_ptr(wp->csd_arm, cpu),
				 kwatch_hwbp_arm_local, wp);
			INIT_CSD(per_cpu_ptr(wp->csd_disarm, cpu),
				 kwatch_hwbp_disarm_local, wp);
		}

		wp->teardown = false;

		hw_breakpoint_init(&wp->attr);
		wp->attr.bp_addr = (unsigned long)&kwatch_dummy_holder;
		wp->attr.bp_len = sizeof(unsigned long);
		/* kwatch localizes corruption: it always watches for writes. */
		wp->attr.bp_type = HW_BREAKPOINT_W;

		wp->event = register_wide_hw_breakpoint(&wp->attr,
							kwatch_hwbp_handler,
							wp);
		if (IS_ERR_PCPU(wp->event)) {
			free_percpu(wp->csd_arm);
			free_percpu(wp->csd_disarm);
			kfree(wp);
			break;
		}

		atomic_set(&wp->refcount, 1);

		mutex_lock(&kwatch_all_wp_mutex);
		list_add(&wp->list, &kwatch_all_wp_list);
		mutex_unlock(&kwatch_all_wp_mutex);
		success++;
	}

	if (!success)
		return -EBUSY;

	/*
	 * A fresh prealloc must start from an empty slot array; warn if a
	 * previous session was not torn down, since refilling without a reset
	 * would index past the freshly sized array.
	 */
	WARN_ON_ONCE(kwatch_wp_slots || kwatch_wp_nr);
	kwatch_wp_nr = 0;

	kwatch_wp_slots = kcalloc(success, sizeof(*kwatch_wp_slots),
				  GFP_KERNEL);
	if (!kwatch_wp_slots) {
		kwatch_hwbp_free();
		return -ENOMEM;
	}
	mutex_lock(&kwatch_all_wp_mutex);
	list_for_each_entry(wp, &kwatch_all_wp_list, list)
		kwatch_wp_slots[kwatch_wp_nr++] = wp;
	mutex_unlock(&kwatch_all_wp_mutex);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "kwatch:online",
					kwatch_hwbp_cpu_online,
					kwatch_hwbp_cpu_offline);
	if (ret < 0) {
		kwatch_hwbp_free();
		return ret;
	}

	kwatch_hwbp_cpuhp_state = ret;
	return 0;
}
