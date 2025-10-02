// SPDX-License-Identifier: GPL-2.0
/*
 * A BPF program for testing DSQ operations including create, destroy,
 * and peek operations. Uses a hybrid approach:
 * - Syscall program for DSQ lifecycle (create/destroy)
 * - Struct ops scheduler for task insertion/dequeue testing
 *
 * Copyright (c) 2025 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2025 Ryan Newton <ryan.newton@alum.mit.edu>
 */

#include <scx/common.bpf.h>
#include <scx/compat.bpf.h>

char _license[] SEC("license") = "GPL";

/* Global variables to store test results */
int dsq_create_result = -1;
int dsq_destroy_result = -1;
int dsq_peek_result1 = -1;
long dsq_inserted_pid = -1;
int insert_test_cpu = -1; /* Set to the cpu that performs the test */
long dsq_peek_result2 = -1;
long dsq_peek_result2_pid = -1;
long dsq_peek_result2_expected = -1;
int test_dsq_id = 1234; /* Use a simple ID like create_dsq example */
int real_dsq_id = 1235; /* DSQ for normal operation */
int enqueue_count = -1;
int dispatch_count = -1;
int debug_ksym_exists = -1;


/* Test if we're actually using the native or compat version */
int check_dsq_insert_ksym(void)
{
	return bpf_ksym_exists(scx_bpf_dsq_insert) ? 1 : 0;
}

int check_dsq_peek_ksym(void)
{
	return bpf_ksym_exists(scx_bpf_dsq_peek) ? 1 : 0;
}

/* Struct_ops scheduler for testing DSQ peek operations */
void BPF_STRUCT_OPS(peek_dsq_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_struct *peek_result;
	int last_insert_test_cpu, cpu;

	enqueue_count++;
	cpu = bpf_get_smp_processor_id();
	last_insert_test_cpu = __sync_val_compare_and_swap(
		&insert_test_cpu, -1, cpu);

	/* On the first task, just do the empty DSQ test and insert into test DSQ */
	if (last_insert_test_cpu == -1) {
		bpf_printk("peek_dsq_enqueue beginning peek test on cpu %d\n", cpu);

		/* Test 1: Peek empty DSQ - should return NULL */
		peek_result = __COMPAT_scx_bpf_dsq_peek(test_dsq_id);
		dsq_peek_result1 = (long)peek_result; /* Should be 0 (NULL) */

		/* Test 2: Insert task into test DSQ for testing in dispatch callback */
		dsq_inserted_pid = p->pid;
		scx_bpf_dsq_insert(p, test_dsq_id, 0, enq_flags);
		dsq_peek_result2_expected = (long)p; /* Expected the task we just inserted */
	} else
		scx_bpf_dsq_insert(p, real_dsq_id, 0, enq_flags);
}

void BPF_STRUCT_OPS(peek_dsq_dispatch, s32 cpu, struct task_struct *prev)
{
	dispatch_count++;
	/* Complete the peek test if we inserted a task but haven't tested peek yet */
	if (insert_test_cpu == cpu && dsq_peek_result2 == -1) {
		struct task_struct *peek_result;

		bpf_printk("peek_dsq_dispatch completing second half of peek test on cpu %d\n",
			   cpu);

		/* Test 3: Peek DSQ after insert - should return the task we inserted */
		peek_result = __COMPAT_scx_bpf_dsq_peek(test_dsq_id);
		/* Store the PID of the peeked task for comparison */
		dsq_peek_result2 = (long)peek_result;
		dsq_peek_result2_pid = peek_result ? peek_result->pid : -1;

		/* Now consume the task since we've peeked at it */
		scx_bpf_dsq_move_to_local(test_dsq_id);
	} else
		scx_bpf_dsq_move_to_local(real_dsq_id);
}

s32 BPF_STRUCT_OPS_SLEEPABLE(peek_dsq_init)
{
	s32 err;

	/* Always set debug values so we can see which version we're using */
	debug_ksym_exists = check_dsq_peek_ksym();

	/* Initialize state first */
	insert_test_cpu = -1;
	enqueue_count = 0;
	dsq_create_result = 0; /* Reset to 0 before attempting */

	/* Create a DSQ */
	err = scx_bpf_create_dsq(test_dsq_id, -1);
	if (!err)
		err = scx_bpf_create_dsq(real_dsq_id, -1);
	if (err) {
		dsq_create_result = err;
		scx_bpf_error("Failed to create DSQ %d: %d", test_dsq_id, err);
		return err;
	}

	dsq_create_result = 1; /* Success */

	return 0;
}

void BPF_STRUCT_OPS(peek_dsq_exit, struct scx_exit_info *ei)
{
	scx_bpf_destroy_dsq(test_dsq_id);
	dsq_destroy_result = 1;
}

SEC(".struct_ops.link")
struct sched_ext_ops peek_dsq_ops = {
	.enqueue = (void *)peek_dsq_enqueue,
	.dispatch = (void *)peek_dsq_dispatch,
	.init = (void *)peek_dsq_init,
	.exit = (void *)peek_dsq_exit,
	.name = "peek_dsq",
};
