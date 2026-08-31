// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"

#define RV_MON_TYPE RV_MON_PER_CPU
#include "nohz.h"
#include <rv/da_monitor.h>

/*
 * This monitor is broken on purpose to test errors, sched_tick can run with
 * stopped ticks for one last time (deferred tick reprogram).
 * A way to fix the monitor is to allow a single optional sched_tick from the
 * stopped state in the model.
 */
SEC("fentry/sched_tick")
int BPF_PROG(handle_sched_tick)
{
	da_handle_start_event(sched_tick_nohz);
	return 0;
}

SEC("fentry/tick_nohz_restart_sched_tick")
int BPF_PROG(handle_tick_restart)
{
	da_handle_start_event(tick_restart_nohz);
	return 0;
}

SEC("tp_btf/tick_stop")
int BPF_PROG(handle_tick_stop, int success, int dependency)
{
	if (success)
		da_handle_event(tick_stop_nohz);
	return 0;
}

SEC(".struct_ops.link")
struct rv_monitor rv_nohz_kern = {
	.name = "nohz",
	.description = "tick does not run when stopped.",
	.enable = da_monitor_enable_bpf,
	.disable = da_monitor_disable_bpf,
	.reset = da_monitor_reset_bpf,
	.enabled = 0,
};

char LICENSE[] SEC("license") = "GPL";
