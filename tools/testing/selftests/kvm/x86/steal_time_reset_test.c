// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that resetting a vCPU and re-enabling KVM steal time on the same
 * vCPU fd does not corrupt the accumulated steal-time value when the vCPU
 * is re-added on a new host thread.
 */
#include <pthread.h>
#include <asm/kvm_para.h>
#include "kvm_util.h"
#include "processor.h"

#define ST_GPA_BASE		(1 << 30)

static void *st_gva;
static u64 guest_stolen_time;
static u64 main_steal;
static u64 thread_steal;

static void guest_code(void)
{
	struct kvm_steal_time *st = st_gva;

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->steal));
	GUEST_SYNC(0);
	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->steal));
	GUEST_DONE();
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_ASSERT(false, "Unexpected exit: %s",
			    exit_reason_str(vcpu->run->exit_reason));
	}
}

static void *do_steal_time(void *arg)
{
	struct timespec ts, stop;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	stop = timespec_add_ns(ts, 100 * MIN_RUN_DELAY_NS);

	while (1) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (timespec_to_ns(timespec_sub(ts, stop)) >= 0)
			break;
	}

	return NULL;
}

static void *vcpu_thread(void *arg)
{
	struct kvm_vcpu *vcpu = arg;

	run_vcpu(vcpu);
	sync_global_from_guest(vcpu->vm, guest_stolen_time);
	thread_steal = guest_stolen_time;

	return NULL;
}

int main(void)
{
	struct kvm_x86_state *reset_state;
	struct kvm_steal_time *st;
	struct kvm_vcpu *vcpu;
	pthread_attr_t attr;
	struct kvm_vm *vm;
	pthread_t thread;
	cpu_set_t cpuset;
	long run_delay;

	ksft_print_header();
	ksft_set_plan(1);

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_KVM_STEAL_TIME));

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_attr_init(&attr);
	pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, ST_GPA_BASE, 1, 1, 0);
	virt_map(vm, ST_GPA_BASE, ST_GPA_BASE, 1);

	st_gva = (void *)ST_GPA_BASE;
	sync_global_to_guest(vm, st_gva);

	st = addr_gva2hva(vm, ST_GPA_BASE);
	memset(st, 0, sizeof(*st));

	reset_state = vcpu_save_state(vcpu);

	vcpu_set_msr(vcpu, MSR_KVM_STEAL_TIME, ST_GPA_BASE | KVM_MSR_ENABLED);
	run_vcpu(vcpu);

	run_delay = get_run_delay();
	pthread_create(&thread, &attr, do_steal_time, NULL);

	while (get_run_delay() - run_delay < MIN_RUN_DELAY_NS)
		sched_yield();

	pthread_join(thread, NULL);
	run_delay = get_run_delay() - run_delay;
	TEST_ASSERT(run_delay >= MIN_RUN_DELAY_NS,
		    "Expected run_delay >= %lu, got %ld",
		    MIN_RUN_DELAY_NS, run_delay);

	run_vcpu(vcpu);
	sync_global_from_guest(vm, guest_stolen_time);
	main_steal = guest_stolen_time;

	vcpu_load_state(vcpu, reset_state);
	vcpu_set_msr(vcpu, MSR_KVM_STEAL_TIME, ST_GPA_BASE | KVM_MSR_ENABLED);

	pthread_create(&thread, NULL, vcpu_thread, vcpu);

	pthread_join(thread, NULL);
	TEST_ASSERT(thread_steal >= main_steal &&
		    thread_steal - main_steal < (1ULL << 63),
		    "Expected sane steal in new vCPU thread: main=%"PRIu64", thread=%"PRIu64,
		    main_steal, thread_steal);
	ksft_test_result_pass("reset preserved steal time across threads\n");

	pthread_attr_destroy(&attr);
	kvm_x86_state_cleanup(reset_state);
	kvm_vm_free(vm);
	ksft_finished();
}
