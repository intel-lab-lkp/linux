/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF scheduler for concurrent pressure testing
 *
 * Copyright (c) 2025 Linux Kernel Contributors
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/* Shared data for statistics */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} pressure_stats SEC(".maps");

/* Per-CPU counters for load tracking */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} per_cpu_ops SEC(".maps");

/* Global operation counter */
static __u64 total_ops = 0;

/* Helper to increment operation counter */
static void increment_ops(void)
{
	__sync_fetch_and_add(&total_ops, 1);
	
	/* Update per-CPU counter */
	u32 key = 0;
	u64 *cpu_count = bpf_map_lookup_elem(&per_cpu_ops, &key);
	if (cpu_count) {
		(*cpu_count)++;
	}
}

/* Helper to update global stats */
static void update_stats(void)
{
	u32 key = 0;
	u64 *stats = bpf_map_lookup_elem(&pressure_stats, &key);
	if (stats) {
		*stats = total_ops;
	}
}

/* Test: High-frequency enqueue operations */
void BPF_STRUCT_OPS(pressure_enqueue, struct task_struct *p, u64 enq_flags)
{
	increment_ops();
	
	/* Test: Insert with varying priorities */
	u64 slice = SCX_SLICE_DFL;
	
	/* Randomize DSQ selection for stress testing */
	if (enq_flags & 0x1) {
		scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, slice, enq_flags);
	} else {
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, enq_flags);
	}
}

/* Test: Rapid dispatch operations */
void BPF_STRUCT_OPS(pressure_dispatch, s32 cpu, struct task_struct *prev)
{
	increment_ops();
	
	/* Test: Multiple dispatch attempts */
	for (int i = 0; i < 3; i++) {
		scx_bpf_dsq_move_to_local(SCX_DSQ_GLOBAL);
		scx_bpf_dsq_move_to_local(SCX_DSQ_LOCAL);
	}
	
	/* Update stats periodically */
	if (total_ops % 100 == 0) {
		update_stats();
	}
}

/* Test: Frequent running/stopping transitions */
void BPF_STRUCT_OPS(pressure_running, struct task_struct *p)
{
	increment_ops();
}

void BPF_STRUCT_OPS(pressure_stopping, struct task_struct *p, bool runnable)
{
	increment_ops();
}

/* Test: Idle state tracking under pressure */
void BPF_STRUCT_OPS(pressure_update_idle, s32 cpu, bool idle)
{
	increment_ops();
	
	/* Validate CPU ID */
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		scx_bpf_error("Invalid CPU in update_idle: %d", cpu);
		return;
	}
}

/* Test: Weight changes under load */
void BPF_STRUCT_OPS(pressure_set_weight, struct task_struct *p, u32 weight)
{
	increment_ops();
	
	/* Validate weight range */
	if (weight < 1 || weight > 10000) {
		scx_bpf_error("Invalid weight: %u", weight);
		return;
	}
}

/* Test: CPU mask changes under pressure */
void BPF_STRUCT_OPS(pressure_set_cpumask, struct task_struct *p,
		   const struct cpumask *cpumask)
{
	increment_ops();
	
	if (!cpumask || bpf_cpumask_empty(cpumask)) {
		scx_bpf_error("Invalid cpumask in set_cpumask");
		return;
	}
}

/* Test: Yield operations under pressure */
bool BPF_STRUCT_OPS(pressure_yield, struct task_struct *from,
		   struct task_struct *to)
{
	increment_ops();
	
	/* Always allow yield in pressure test */
	return true;
}

/* Test: Initialization with performance tracking */
s32 BPF_STRUCT_OPS_SLEEPABLE(pressure_init)
{
	total_ops = 0;
	
	/* Initialize stats map */
	u32 key = 0;
	u64 initial = 0;
	bpf_map_update_elem(&pressure_stats, &key, &initial, BPF_ANY);
	
	return 0;
}

/* Test: Exit with statistics reporting */
void BPF_STRUCT_OPS(pressure_exit, struct scx_exit_info *info)
{
	update_stats();
	
	/* Log final statistics */
	if (info) {
		scx_bpf_print("Pressure test completed: %llu total operations\n", 
			     total_ops);
	}
}

SEC(".struct_ops.link")
struct sched_ext_ops pressure_ops = {
	.enqueue		= (void *) pressure_enqueue,
	.dispatch		= (void *) pressure_dispatch,
	.running		= (void *) pressure_running,
	.stopping		= (void *) pressure_stopping,
	.update_idle		= (void *) pressure_update_idle,
	.set_weight		= (void *) pressure_set_weight,
	.set_cpumask		= (void *) pressure_set_cpumask,
	.yield			= (void *) pressure_yield,
	.init			= (void *) pressure_init,
	.exit			= (void *) pressure_exit,
	.name			= "concurrent_pressure",
};