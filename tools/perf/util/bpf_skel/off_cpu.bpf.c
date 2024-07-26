// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
// Copyright (c) 2022 Google
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* task->flags for off-cpu analysis */
#define PF_KTHREAD   0x00200000  /* I am a kernel thread */

/* task->state for off-cpu analysis */
#define TASK_INTERRUPTIBLE	0x0001
#define TASK_UNINTERRUPTIBLE	0x0002

/* create a new thread */
#define CLONE_THREAD  0x10000

#define MAX_STACKS   32
#define MAX_ENTRIES  102400

#define MAX_CPUS  4096
#define MAX_OFFCPU_LEN 128

struct tstamp_data {
	__u32 stack_id;
	__u32 state;
	__u64 timestamp;
};

struct offcpu_key {
	__u32 pid;
	__u32 tgid;
	__u32 stack_id;
	__u32 state;
	__u64 cgroup_id;
};

/* for dumping at the end */
struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, MAX_STACKS * sizeof(__u64));
	__uint(max_entries, MAX_ENTRIES);
} stacks SEC(".maps");

struct offcpu_data {
	u64 array[MAX_OFFCPU_LEN];
};

struct stack_data {
	u64 array[MAX_STACKS];
};

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u32));
	__uint(max_entries, MAX_CPUS);
} offcpu_output SEC(".maps");

/* temporary offcpu sample */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(struct offcpu_data));
	__uint(max_entries, 1);
} offcpu_payload SEC(".maps");

/* temporary stack data */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(struct stack_data));
	__uint(max_entries, 1);
} stack_tmp SEC(".maps");

/* cached stack per task storage */
struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct stack_data);
} stack_cache SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct tstamp_data);
} tstamp SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(key_size, sizeof(struct offcpu_key));
	__uint(value_size, sizeof(__u64));
	__uint(max_entries, MAX_ENTRIES);
} off_cpu SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u8));
	__uint(max_entries, 1);
} cpu_filter SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u8));
	__uint(max_entries, 1);
} task_filter SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(key_size, sizeof(__u64));
	__uint(value_size, sizeof(__u8));
	__uint(max_entries, 1);
} cgroup_filter SEC(".maps");

/* new kernel task_struct definition */
struct task_struct___new {
	long __state;
} __attribute__((preserve_access_index));

/* old kernel task_struct definition */
struct task_struct___old {
	long state;
} __attribute__((preserve_access_index));

int enabled = 0;
int has_cpu = 0;
int has_task = 0;
int has_cgroup = 0;
int uses_tgid = 0;

const volatile bool has_prev_state = false;
const volatile bool needs_cgroup = false;
const volatile bool uses_cgroup_v1 = false;

int perf_subsys_id = -1;

__u64 sample_id, sample_type, offcpu_thresh;

/*
 * Old kernel used to call it task_struct->state and now it's '__state'.
 * Use BPF CO-RE "ignored suffix rule" to deal with it like below:
 *
 * https://nakryiko.com/posts/bpf-core-reference-guide/#handling-incompatible-field-and-type-changes
 */
static inline int get_task_state(struct task_struct *t)
{
	/* recast pointer to capture new type for compiler */
	struct task_struct___new *t_new = (void *)t;

	if (bpf_core_field_exists(t_new->__state)) {
		return BPF_CORE_READ(t_new, __state);
	} else {
		/* recast pointer to capture old type for compiler */
		struct task_struct___old *t_old = (void *)t;

		return BPF_CORE_READ(t_old, state);
	}
}

static inline __u64 get_cgroup_id(struct task_struct *t)
{
	struct cgroup *cgrp;

	if (!uses_cgroup_v1)
		return BPF_CORE_READ(t, cgroups, dfl_cgrp, kn, id);

	if (perf_subsys_id == -1) {
#if __has_builtin(__builtin_preserve_enum_value)
		perf_subsys_id = bpf_core_enum_value(enum cgroup_subsys_id,
						     perf_event_cgrp_id);
#else
		perf_subsys_id = perf_event_cgrp_id;
#endif
	}

	cgrp = BPF_CORE_READ(t, cgroups, subsys[perf_subsys_id], cgroup);
	return BPF_CORE_READ(cgrp, kn, id);
}

static inline int can_record(struct task_struct *t, int state)
{
	/* kernel threads don't have user stack */
	if (t->flags & PF_KTHREAD)
		return 0;

	if (state != TASK_INTERRUPTIBLE &&
	    state != TASK_UNINTERRUPTIBLE)
		return 0;

	if (has_cpu) {
		__u32 cpu = bpf_get_smp_processor_id();
		__u8 *ok;

		ok = bpf_map_lookup_elem(&cpu_filter, &cpu);
		if (!ok)
			return 0;
	}

	if (has_task) {
		__u8 *ok;
		__u32 pid;

		if (uses_tgid)
			pid = t->tgid;
		else
			pid = t->pid;

		ok = bpf_map_lookup_elem(&task_filter, &pid);
		if (!ok)
			return 0;
	}

	if (has_cgroup) {
		__u8 *ok;
		__u64 cgrp_id = get_cgroup_id(t);

		ok = bpf_map_lookup_elem(&cgroup_filter, &cgrp_id);
		if (!ok)
			return 0;
	}

	return 1;
}

static inline bool check_bounds(int index)
{
	if (index >= 0 && index < MAX_OFFCPU_LEN)
		return true;

	return false;
}

static inline int copy_stack(struct stack_data *from,
			     struct offcpu_data *to, int n)
{
	int max_stacks = MAX_STACKS, len = 0;

	if (!from)
		return len;

	for (int i = 0; i < max_stacks && from->array[i]; ++i) {
		if (check_bounds(n + 2 + i)) {
			to->array[n + 2 + i] = from->array[i];
			++len;
		}
	}
	return len;
}

static int off_cpu_dump(void *ctx, struct offcpu_data *data, struct offcpu_key *key,
			struct stack_data *stack_p, __u64 delta, __u64 timestamp)
{
	int size, n = 0, ip_pos = -1, len = 0;

	if (sample_type & PERF_SAMPLE_IDENTIFIER && check_bounds(n))
		data->array[n++] = sample_id;
	if (sample_type & PERF_SAMPLE_IP && check_bounds(n)) {
		ip_pos = n;
		data->array[n++] = 0;  /* will be updated */
	}
	if (sample_type & PERF_SAMPLE_TID && check_bounds(n))
		data->array[n++] = (u64)key->pid << 32 | key->tgid;
	if (sample_type & PERF_SAMPLE_TIME && check_bounds(n))
		data->array[n++] = timestamp;
	if (sample_type & PERF_SAMPLE_ID && check_bounds(n))
		data->array[n++] = sample_id;
	if (sample_type & PERF_SAMPLE_CPU && check_bounds(n))
		data->array[n++] = 0;
	if (sample_type & PERF_SAMPLE_PERIOD && check_bounds(n))
		data->array[n++] = delta;
	if (sample_type & PERF_SAMPLE_CALLCHAIN && check_bounds(n + 2)) {
		/* data->array[n] is callchain->nr (updated later) */
		data->array[n + 1] = PERF_CONTEXT_USER;
		data->array[n + 2] = 0;

		len = copy_stack(stack_p, data, n);

		/* update length of callchain */
		data->array[n] = len + 1;

		/* update sample ip with the first callchain entry */
		if (ip_pos >= 0)
			data->array[ip_pos] = data->array[n + 2];

		/* calculate sample callchain data->array length */
		n += len + 2;
	}
	if (sample_type & PERF_SAMPLE_CGROUP && check_bounds(n))
		data->array[n++] = key->cgroup_id;

	size = n * sizeof(u64);
	if (size >= 0 && size <= MAX_OFFCPU_LEN * sizeof(u64))
		bpf_perf_event_output(ctx, &offcpu_output, BPF_F_CURRENT_CPU, data, size);

	return 0;
}

static int off_cpu_stat(u64 *ctx, struct task_struct *prev,
			struct task_struct *next, int state)
{
	__u64 ts;
	__u32 stack_id;
	struct tstamp_data *pelem;
	struct stack_data *stack_tmp_p, *stack_p;
	int zero = 0, len = 0;

	ts = bpf_ktime_get_ns();

	if (!can_record(prev, state))
		goto next;

	stack_id = bpf_get_stackid(ctx, &stacks,
				   BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK);

	/* temporary stack data */
	stack_tmp_p = bpf_map_lookup_elem(&stack_tmp, &zero);
	if (stack_tmp_p)
		len = bpf_get_stack(ctx, stack_tmp_p->array, MAX_STACKS * sizeof(u64),
				    BPF_F_USER_STACK) / sizeof(u64);

	/* save stacks if collectable */
	if (len > 0) {
		stack_p = bpf_task_storage_get(&stack_cache, prev, NULL,
					       BPF_LOCAL_STORAGE_GET_F_CREATE);
		if (stack_p) {
			for (int i = 0; i < len && i < MAX_STACKS; ++i)
				stack_p->array[i] = stack_tmp_p->array[i];
		}
	}

	pelem = bpf_task_storage_get(&tstamp, prev, NULL,
				     BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!pelem)
		goto next;

	pelem->timestamp = ts;
	pelem->state = state;
	pelem->stack_id = stack_id;

next:
	pelem = bpf_task_storage_get(&tstamp, next, NULL, 0);

	if (pelem && pelem->timestamp) {
		struct offcpu_key key = {
			.pid = next->pid,
			.tgid = next->tgid,
			.stack_id = pelem->stack_id,
			.state = pelem->state,
			.cgroup_id = needs_cgroup ? get_cgroup_id(next) : 0,
		};
		__u64 delta = ts - pelem->timestamp;
		__u64 *total;

		total = bpf_map_lookup_elem(&off_cpu, &key);
		if (total)
			*total += delta;
		else
			bpf_map_update_elem(&off_cpu, &key, &delta, BPF_ANY);

		if (delta >= offcpu_thresh) {
			struct offcpu_data *data = bpf_map_lookup_elem(&offcpu_payload, &zero);
			stack_p = bpf_task_storage_get(&stack_cache, next, NULL, 0);
			if (data && stack_p)
				off_cpu_dump(ctx, data, &key, stack_p, delta, pelem->timestamp);
		}

		/* prevent to reuse the timestamp later */
		pelem->timestamp = 0;
	}

	return 0;
}

SEC("tp_btf/task_newtask")
int on_newtask(u64 *ctx)
{
	struct task_struct *task;
	u64 clone_flags;
	u32 pid;
	u8 val = 1;

	if (!uses_tgid)
		return 0;

	task = (struct task_struct *)bpf_get_current_task();

	pid = BPF_CORE_READ(task, tgid);
	if (!bpf_map_lookup_elem(&task_filter, &pid))
		return 0;

	task = (struct task_struct *)ctx[0];
	clone_flags = ctx[1];

	pid = task->tgid;
	if (!(clone_flags & CLONE_THREAD))
		bpf_map_update_elem(&task_filter, &pid, &val, BPF_NOEXIST);

	return 0;
}

SEC("tp_btf/sched_switch")
int on_switch(u64 *ctx)
{
	struct task_struct *prev, *next;
	int prev_state;

	if (!enabled)
		return 0;

	prev = (struct task_struct *)ctx[1];
	next = (struct task_struct *)ctx[2];

	if (has_prev_state)
		prev_state = (int)ctx[3];
	else
		prev_state = get_task_state(prev);

	return off_cpu_stat(ctx, prev, next, prev_state & 0xff);
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
