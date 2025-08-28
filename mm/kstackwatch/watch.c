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

static unsigned long long watch_holder;

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
