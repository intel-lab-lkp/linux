// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that KVM resets steal-time accounting when a vCPU fd is run from
 * a different host PID.
 */

#include <pthread.h>
#include <asm/kvm_para.h>
#include "kvm_util.h"
#include "processor.h"

#define ST_GPA_BASE		(1 << 30)
#define ST_SANE_DELTA_NS	(1ULL << 63)

static void *st_gva;
static u64 guest_stolen_time;
static u64 main_steal;
static u64 thread_steal;

#if defined(__x86_64__)

#define STEAL_TIME_SIZE	((sizeof(struct kvm_steal_time) + 63) & ~63)

static void guest_code(void)
{
	struct kvm_steal_time *st = st_gva;

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->steal));
	GUEST_SYNC(0);

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->steal));
	GUEST_SYNC(1);

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->steal));
	GUEST_DONE();
}

static bool steal_time_supported(struct kvm_vcpu *vcpu)
{
	return kvm_cpu_has(X86_FEATURE_KVM_STEAL_TIME);
}

static void steal_time_enable(struct kvm_vcpu *vcpu)
{
	vcpu_set_msr(vcpu, MSR_KVM_STEAL_TIME,
		     (ulong)st_gva | KVM_MSR_ENABLED);
}

#elif defined(__aarch64__)

#define STEAL_TIME_SIZE	((sizeof(struct st_time) + 63) & ~63)

#define PV_TIME_ST	0xc5000021

struct st_time {
	u32 rev;
	u32 attr;
	u64 st_time;
};

static void guest_code(void)
{
	struct arm_smccc_res res;
	struct st_time *st;

	do_smccc(PV_TIME_ST, 0, 0, 0, 0, 0, 0, 0, &res);
	GUEST_ASSERT_NE(res.a0, -1);
	GUEST_ASSERT_EQ(res.a0, (ulong)st_gva);

	st = (struct st_time *)res.a0;
	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->st_time));
	GUEST_SYNC(0);

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->st_time));
	GUEST_SYNC(1);

	WRITE_ONCE(guest_stolen_time, READ_ONCE(st->st_time));
	GUEST_DONE();
}

static bool steal_time_supported(struct kvm_vcpu *vcpu)
{
	struct kvm_device_attr dev = {
		.group = KVM_ARM_VCPU_PVTIME_CTRL,
		.attr = KVM_ARM_VCPU_PVTIME_IPA,
	};

	return !__vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &dev);
}

static void steal_time_enable(struct kvm_vcpu *vcpu)
{
	u64 st_ipa = (ulong)st_gva;
	struct kvm_device_attr dev = {
		.group = KVM_ARM_VCPU_PVTIME_CTRL,
		.attr = KVM_ARM_VCPU_PVTIME_IPA,
		.addr = (u64)&st_ipa,
	};

	vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &dev);
}

#else
#error "steal_time_change_pid is not implemented on this architecture"
#endif

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
	stop = timespec_add_ns(ts, MIN_RUN_DELAY_NS);

	while (timespec_to_ns(timespec_sub(ts, stop)) < 0)
		clock_gettime(CLOCK_MONOTONIC, &ts);

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
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	pthread_attr_t attr;
	pthread_t thread;
	cpu_set_t cpuset;
	long run_delay;
	long run_delay_delta;

	ksft_print_header();
	ksft_set_plan(1);

	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	pthread_attr_init(&attr);
	pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    ST_GPA_BASE, 1, 1, 0);
	virt_map(vm, ST_GPA_BASE, ST_GPA_BASE, 1);

	st_gva = (void *)ST_GPA_BASE;
	sync_global_to_guest(vm, st_gva);
	memset(addr_gva2hva(vm, ST_GPA_BASE), 0, STEAL_TIME_SIZE);

	TEST_REQUIRE(steal_time_supported(vcpu));

	steal_time_enable(vcpu);
	run_vcpu(vcpu);

	run_delay = get_run_delay();
	pthread_create(&thread, &attr, do_steal_time, NULL);

	while (get_run_delay() - run_delay < MIN_RUN_DELAY_NS)
		sched_yield();

	pthread_join(thread, NULL);
	run_delay_delta = get_run_delay() - run_delay;
	TEST_ASSERT(run_delay_delta >= MIN_RUN_DELAY_NS,
		    "Expected run_delay >= %ld, got %ld",
		    MIN_RUN_DELAY_NS, run_delay_delta);

	run_vcpu(vcpu);
	sync_global_from_guest(vm, guest_stolen_time);
	main_steal = guest_stolen_time;

	TEST_ASSERT(main_steal >= MIN_RUN_DELAY_NS,
		    "Expected steal time >= %ld, got %"PRIu64,
		    MIN_RUN_DELAY_NS, main_steal);

	pthread_create(&thread, NULL, vcpu_thread, vcpu);
	pthread_join(thread, NULL);

	TEST_ASSERT(thread_steal >= main_steal &&
		    thread_steal - main_steal < ST_SANE_DELTA_NS,
		    "Expected sane steal after vCPU pid change: "
		    "old=%"PRIu64", new=%"PRIu64,
		    main_steal, thread_steal);

	ksft_test_result_pass("steal time remains sane across vCPU pid change\n");

	pthread_attr_destroy(&attr);
	kvm_vm_free(vm);
	ksft_finished();
}
