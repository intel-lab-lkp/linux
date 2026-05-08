// SPDX-License-Identifier: GPL-2.0
/*
 * scx_ai_numa - AI NUMA-aware scheduler (BPF side)
 *
 * Scheduling policy optimized for AI training workloads:
 *
 * 1. Per-NUMA-node DSQs: each NUMA node owns a dedicated dispatch queue.
 *    Tasks are steered to the DSQ of the NUMA node they last ran on,
 *    preserving L3 cache warmth and reducing remote DRAM accesses that
 *    stall GPU kernel launches waiting on CPU preprocessing.
 *
 * 2. Idle fast path: when an idle CPU is found, bypass the per-node DSQ
 *    and insert directly into SCX_DSQ_LOCAL for minimum latency.
 *
 * 3. Task NUMA affinity: per-task storage tracks the preferred NUMA node
 *    (updated every time select_cpu() sees the task's prev_cpu).
 *
 * 4. Work stealing: if a node's DSQ is empty, try remote nodes in order
 *    to prevent CPU starvation during load imbalance (e.g., bursty GPU
 *    command submissions landing on a single NUMA node).
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

#define MAX_NUMA_NODES 16

/* One DSQ per NUMA node, IDs 0 .. MAX_NUMA_NODES-1 */
#define NUMA_DSQ(node) ((u64)(node))

/* Per-task context: remember which NUMA node this task prefers */
struct task_ctx {
	u32 preferred_node;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/* Per-node counters (per-CPU to avoid false sharing) */
struct node_stat {
	__u64 local_dsq;	/* fast-path: direct SCX_DSQ_LOCAL insert */
	__u64 numa_dsq;		/* enqueued to per-node DSQ */
	__u64 steal;		/* dispatched from a remote node's DSQ */
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(struct node_stat));
	__uint(max_entries, MAX_NUMA_NODES);
} node_stats SEC(".maps");

/* Set by userspace after detecting the number of NUMA nodes */
const volatile u32 nr_nodes = 1;

static __always_inline u32 cpu_to_node(s32 cpu)
{
	return __COMPAT_scx_bpf_cpu_node(cpu);
}

static __always_inline void stat_inc_local(u32 node)
{
	struct node_stat *s = bpf_map_lookup_elem(&node_stats, &node);

	if (s)
		s->local_dsq++;
}

static __always_inline void stat_inc_numa(u32 node)
{
	struct node_stat *s = bpf_map_lookup_elem(&node_stats, &node);

	if (s)
		s->numa_dsq++;
}

static __always_inline void stat_inc_steal(u32 node)
{
	struct node_stat *s = bpf_map_lookup_elem(&node_stats, &node);

	if (s)
		s->steal++;
}

s32 BPF_STRUCT_OPS(ai_numa_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	struct task_ctx *tctx;
	bool is_idle = false;
	u32 node;
	s32 cpu;

	/* Update task's preferred NUMA node from prev_cpu */
	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				     BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (tctx) {
		node = cpu_to_node(prev_cpu);
		tctx->preferred_node = node < nr_nodes ? node : 0;
	}

	/*
	 * Default selection tries prev_cpu first (same LLC), which preserves
	 * L1/L2/L3 cache across AI loop iterations without extra policy code.
	 */
	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		/* Idle CPU found: bypass DSQ for minimum latency */
		node = cpu_to_node(cpu);
		stat_inc_local(node);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(ai_numa_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;
	u32 node = 0;

	/*
	 * Route to the task's preferred NUMA node DSQ.
	 * Keeping AI tasks on the same NUMA node as their GPU's host memory
	 * reduces cross-node DRAM traffic and PCIe DMA stalls.
	 */
	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
	if (tctx) {
		node = tctx->preferred_node;
		if (node >= nr_nodes)
			node = 0;
	}

	stat_inc_numa(node);
	scx_bpf_dsq_insert(p, NUMA_DSQ(node), SCX_SLICE_DFL, enq_flags);
}

void BPF_STRUCT_OPS(ai_numa_dispatch, s32 cpu, struct task_struct *prev)
{
	u32 my_node = cpu_to_node(cpu);
	u32 i;

	/* First: consume from our own NUMA node — zero cross-node traffic */
	if (scx_bpf_dsq_move_to_local(NUMA_DSQ(my_node), 0))
		return;

	/*
	 * Work steal from other nodes in order.
	 * Prevents CPU starvation when one GPU's launch bursts all tasks
	 * onto a single NUMA node while other nodes sit idle.
	 */
	for (i = 0; i < MAX_NUMA_NODES; i++) {
		u32 node = i;

		if (node >= nr_nodes)
			break;
		if (node == my_node)
			continue;
		if (scx_bpf_dsq_move_to_local(NUMA_DSQ(node), 0)) {
			stat_inc_steal(my_node);
			return;
		}
	}
}

s32 BPF_STRUCT_OPS_SLEEPABLE(ai_numa_init)
{
	u32 i;
	int ret;

	for (i = 0; i < MAX_NUMA_NODES; i++) {
		if (i >= nr_nodes)
			break;
		ret = scx_bpf_create_dsq(NUMA_DSQ(i), -1);
		if (ret) {
			scx_bpf_error("failed to create DSQ for node %u: %d",
				      i, ret);
			return ret;
		}
	}

	return 0;
}

void BPF_STRUCT_OPS(ai_numa_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(ai_numa_ops,
	       .select_cpu	= (void *)ai_numa_select_cpu,
	       .enqueue		= (void *)ai_numa_enqueue,
	       .dispatch	= (void *)ai_numa_dispatch,
	       .init		= (void *)ai_numa_init,
	       .exit		= (void *)ai_numa_exit,
	       .name		= "ai_numa");
