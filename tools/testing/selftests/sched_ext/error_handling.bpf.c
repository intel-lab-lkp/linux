/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF scheduler for error handling testing
 *
 * Copyright (c) 2025 Linux Kernel Contributors
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

/* Error statistics structure */
struct error_stats {
	__u32 invalid_task_count;
	__u32 invalid_cpu_count;
	__u32 invalid_weight_count;
	__u32 null_pointer_count;
	__u32 boundary_violation_count;
};

/* Map to store error statistics */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct error_stats);
} error_stats SEC(".maps");

/* Helper to increment error counter */
static void increment_error_counter(__u32 counter_type)
{
	u32 key = 0;
	struct error_stats *stats = bpf_map_lookup_elem(&error_stats, &key);
	if (!stats)
		return;

	switch (counter_type) {
	case 0: /* invalid_task */
		__sync_fetch_and_add(&stats->invalid_task_count, 1);
		break;
	case 1: /* invalid_cpu */
		__sync_fetch_and_add(&stats->invalid_cpu_count, 1);
		break;
	case 2: /* invalid_weight */
		__sync_fetch_and_add(&stats->invalid_weight_count, 1);
		break;
	case 3: /* null_pointer */
		__sync_fetch_and_add(&stats->null_pointer_count, 1);
		break;
	case 4: /* boundary_violation */
		__sync_fetch_and_add(&stats->boundary_violation_count, 1);
		break;
	}
}

/* Test: Handle invalid task in enqueue */
void BPF_STRUCT_OPS(error_ops_enqueue, struct task_struct *p, u64 enq_flags)
{
	/* Test: Check for NULL task */
	if (!p) {
		increment_error_counter(3); /* null_pointer */
		scx_bpf_error("NULL task in enqueue");
		return;
	}

	/* Test: Check task state validity */
	if (p->state & TASK_DEAD) {
		increment_error_counter(0); /* invalid_task */
		scx_bpf_error("Dead task in enqueue");
		return;
	}

	/* Normal operation */
	scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, enq_flags);
}

/* Test: Handle invalid CPU in dispatch */
void BPF_STRUCT_OPS(error_ops_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Test: Validate CPU ID */
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		increment_error_counter(1); /* invalid_cpu */
		scx_bpf_error("Invalid CPU in dispatch: %d", cpu);
		return;
	}

	/* Test: Handle NULL prev (allowed in some cases) */
	if (prev && (prev->state & TASK_DEAD)) {
		increment_error_counter(0); /* invalid_task */
		/* Continue anyway - prev is being switched out */
	}

	/* Normal dispatch */
	scx_bpf_dsq_move_to_local(SCX_DSQ_GLOBAL);
}

/* Test: Validate running task */
void BPF_STRUCT_OPS(error_ops_running, struct task_struct *p)
{
	if (!p) {
		increment_error_counter(3); /* null_pointer */
		scx_bpf_error("NULL task in running");
		return;
	}

	/* Check for invalid states */
	if (p->state & (TASK_DEAD | TASK_WAKING)) {
		increment_error_counter(0); /* invalid_task */
		scx_bpf_error("Invalid task state in running: 0x%lx", p->state);
		return;
	}
}

/* Test: Validate stopping task */
void BPF_STRUCT_OPS(error_ops_stopping, struct task_struct *p, bool runnable)
{
	if (!p) {
		increment_error_counter(3); /* null_pointer */
		scx_bpf_error("NULL task in stopping");
		return;
	}
}

/* Test: Boundary conditions in update_idle */
void BPF_STRUCT_OPS(error_ops_update_idle, s32 cpu, bool idle)
{
	/* Test: CPU boundary check */
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		increment_error_counter(1); /* invalid_cpu */
		scx_bpf_error("CPU out of bounds: %d", cpu);
		return;
	}

	/* Test: Extreme CPU values */
	if (cpu == INT_MAX || cpu == INT_MIN) {
		increment_error_counter(4); /* boundary_violation */
		scx_bpf_error("Extreme CPU value: %d", cpu);
		return;
	}
}

/* Test: Weight boundary validation */
void BPF_STRUCT_OPS(error_ops_set_weight, struct task_struct *p, u32 weight)
{
	if (!p) {
		increment_error_counter(3); /* null_pointer */
		return;
	}

	/* Test: Weight bounds */
	if (weight < 1) {
		increment_error_counter(2); /* invalid_weight */
		scx_bpf_error("Weight too low: %u", weight);
		return;
	}

	if (weight > 10000) {
		increment_error_counter(2); /* invalid_weight */
		scx_bpf_error("Weight too high: %u", weight);
		return;
	}

	/* Test: Boundary values */
	if (weight == 0 || weight == 10001) {
		increment_error_counter(4); /* boundary_violation */
	}
}

/* Test: CPU mask validation */
void BPF_STRUCT_OPS(error_ops_set_cpumask, struct task_struct *p,
		   const struct cpumask *cpumask)
{
	if (!p) {
		increment_error_counter(3); /* null_pointer */
		return;
	}

	if (!cpumask) {
		increment_error_counter(3); /* null_pointer */
		scx_bpf_error("NULL cpumask");
		return;
	}

	/* Test: Empty cpumask */
	if (bpf_cpumask_empty(cpumask)) {
		increment_error_counter(4); /* boundary_violation */
		scx_bpf_error("Empty cpumask");
		return;
	}
}

/* Test: Yield with invalid tasks */
bool BPF_STRUCT_OPS(error_ops_yield, struct task_struct *from,
		   struct task_struct *to)
{
	/* Test: NULL from task (should never happen) */
	if (!from) {
		increment_error_counter(3); /* null_pointer */
		scx_bpf_error("NULL from task in yield");
		return false;
	}

	/* Test: NULL to task is allowed (yield to any) */
	if (!to) {
		return true;
	}

	/* Test: Both tasks valid */
	return true;
}

/* Test: Initialization with error handling */
s32 BPF_STRUCT_OPS_SLEEPABLE(error_ops_init)
{
	/* Initialize error stats */
	u32 key = 0;
	struct error_stats initial = {0};
	
	int ret = bpf_map_update_elem(&error_stats, &key, &initial, BPF_ANY);
	if (ret < 0) {
		scx_bpf_error("Failed to initialize error stats: %d", ret);
		return ret;
	}

	/* Test: Create DSQ with various parameters */
	ret = scx_bpf_create_dsq(1, -1);
	if (ret < 0) {
		scx_bpf_error("Failed to create DSQ: %d", ret);
		return ret;
	}

	return 0;
}

/* Test: Exit with error summary */
void BPF_STRUCT_OPS(error_ops_exit, struct scx_exit_info *info)
{
	u32 key = 0;
	struct error_stats *stats = bpf_map_lookup_elem(&error_stats, &key);
	
	if (stats) {
		scx_bpf_print("Error handling summary:\n");
		scx_bpf_print("  Invalid tasks: %u\n", stats->invalid_task_count);
		scx_bpf_print("  Invalid CPUs: %u\n", stats->invalid_cpu_count);
		scx_bpf_print("  Invalid weights: %u\n", stats->invalid_weight_count);
		scx_bpf_print("  NULL pointers: %u\n", stats->null_pointer_count);
		scx_bpf_print("  Boundary violations: %u\n", stats->boundary_violation_count);
	}
}

SEC(".struct_ops.link")
struct sched_ext_ops error_ops = {
	.enqueue		= (void *) error_ops_enqueue,
	.dispatch		= (void *) error_ops_dispatch,
	.running		= (void *) error_ops_running,
	.stopping		= (void *) error_ops_stopping,
	.update_idle		= (void *) error_ops_update_idle,
	.set_weight		= (void *) error_ops_set_weight,
	.set_cpumask		= (void *) error_ops_set_cpumask,
	.yield			= (void *) error_ops_yield,
	.init			= (void *) error_ops_init,
	.exit			= (void *) error_ops_exit,
	.name			= "error_handling",
};