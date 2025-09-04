// SPDX-License-Identifier: GPL-2.0

#include <linux/hw_breakpoint.h>
#include <linux/kern_levels.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/perf_event.h>
#include <linux/sched/debug.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stacktrace.h>

#include "kstackwatch.h"

#define MAX_STACK_ENTRIES 64

struct perf_event *__percpu *watch_events;
struct ksw_config *watch_config;
static DEFINE_SPINLOCK(watch_lock);

static unsigned long long watch_holder;

static struct watch_info {
	u64 addr;
	u64 len;
} watch_info;

static void ksw_watch_on_local_cpu(void *info);

static DEFINE_PER_CPU(call_single_data_t,
		      watch_csd) = CSD_INIT(ksw_watch_on_local_cpu,
					    &watch_info);

static void ksw_watch_handler(struct perf_event *bp,
			      struct perf_sample_data *data,
			      struct pt_regs *regs)
{
	pr_err("========== KStackWatch: Caught stack corruption =======\n");
	pr_err("KSW: config %s\n", watch_config->config_str);
	show_regs(regs);
	pr_err("=================== KStackWatch End ==================\n");

	if (panic_on_catch)
		panic("KSW: Stack corruption detected");
}

/*
 * set up watchon current CPU
 * addr and len updated by ksw_watch_on() already
 */
static void ksw_watch_on_local_cpu(void *data)
{
	struct perf_event *bp;
	struct perf_event_attr attr;
	struct watch_info *watch_info = data;

	int cpu = smp_processor_id();
	int ret;

	bp = *per_cpu_ptr(watch_events, cpu);
	if (!bp)
		return;
	attr.bp_addr = watch_info->addr;
	attr.bp_len = watch_info->len;
	attr.bp_type = bp->attr.bp_type;

	ret = hw_breakpoint_modify_local(bp, &attr);
	if (ret) {
		pr_err("KSW: failed to reinstall HWBP on CPU %d ret %d\n", cpu,
		       ret);
		return;
	}

	if (bp->attr.bp_addr == (unsigned long)&watch_holder) {
		pr_debug("KSW: watch off CPU %d\n", cpu);
	} else {
		pr_debug("KSW: watch on CPU %d at 0x%px (len %llu)\n", cpu,
			 (void *)bp->attr.bp_addr, bp->attr.bp_len);
	}
}

int ksw_watch_on(u64 watch_addr, u64 watch_len)
{
	unsigned long flags;
	int cpu;

	if (!watch_addr) {
		pr_err("KSW: watch with invalid address\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&watch_lock, flags);

	/*
	 * check if already watched
	 */
	if (watch_info.addr != 0 && // not uninit
	    watch_info.addr != (unsigned long)&watch_holder && // installed
	    watch_addr != (unsigned long)&watch_holder) { //not restore
		spin_unlock_irqrestore(&watch_lock, flags);
		return -EBUSY;
	}

	watch_info.addr = watch_addr;
	watch_info.len = watch_len;

	spin_unlock_irqrestore(&watch_lock, flags);

	if (watch_addr == (unsigned long)&watch_holder)
		pr_debug("KSW: watch off starting\n");
	else
		pr_debug("KSW: watch on starting\n");

	for_each_online_cpu(cpu) {
		if (cpu == raw_smp_processor_id()) {
			ksw_watch_on_local_cpu(&watch_info);
		} else {
			call_single_data_t *csd = &per_cpu(watch_csd, cpu);

			smp_call_function_single_async(cpu, csd);
		}
	}

	return 0;
}

void ksw_watch_off(void)
{
	ksw_watch_on((unsigned long)&watch_holder, sizeof(watch_holder));
}

int ksw_watch_init(struct ksw_config *config)
{
	struct perf_event_attr attr;

	hw_breakpoint_init(&attr);
	attr.bp_addr = (unsigned long)&watch_holder;
	attr.bp_len = HW_BREAKPOINT_LEN_8;
	attr.bp_type = HW_BREAKPOINT_W;
	watch_events =
		register_wide_hw_breakpoint(&attr, ksw_watch_handler, NULL);
	if (IS_ERR((void *)watch_events)) {
		int ret = PTR_ERR((void *)watch_events);

		pr_err("KSW: failed to register wide hw breakpoint: %d\n", ret);
		return ret;
	}

	watch_config = config;
	pr_info("KSW: watch inited\n");
	return 0;
}

void ksw_watch_exit(void)
{
	unregister_wide_hw_breakpoint(watch_events);
	watch_events = NULL;

	pr_info("KSW: watch exited\n");
}
