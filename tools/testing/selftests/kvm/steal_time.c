// SPDX-License-Identifier: GPL-2.0
/*
 * steal/stolen time test
 *
 * Copyright (C) 2020, Red Hat, Inc.
 */
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <linux/bitmap.h>
#include <linux/kernel.h>
#include <asm/kvm.h>
#ifdef __riscv
#include "sbi.h"
#else
#include <asm/kvm_para.h>
#endif

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "ucall_common.h"

#define NR_VCPUS		4
#define ST_GPA_BASE		(1 << 30)

static void *st_gva[NR_VCPUS];
static u64 guest_stolen_time[NR_VCPUS];

static struct kvm_vm *vm_create_steal_time(u32 nr_vcpus, void *guest_code,
					   struct kvm_vcpu *vcpus[]);
static long force_steal_time(void);

#if defined(__x86_64__)

/* steal_time must have 64-byte alignment */
#define STEAL_TIME_SIZE		((sizeof(struct kvm_steal_time) + 63) & ~63)
#define NR_LIFECYCLE_TESTS	0

static void check_steal_time_lifecycle(void)
{
}

static void check_status(struct kvm_steal_time *st)
{
	GUEST_ASSERT(!(READ_ONCE(st->version) & 1));
	GUEST_ASSERT_EQ(READ_ONCE(st->flags), 0);
	GUEST_ASSERT_EQ(READ_ONCE(st->preempted), 0);
}

static void guest_code(int cpu)
{
	struct kvm_steal_time *st = st_gva[cpu];
	u32 version;

	GUEST_ASSERT_EQ(rdmsr(MSR_KVM_STEAL_TIME), ((u64)st_gva[cpu] | KVM_MSR_ENABLED));

	memset(st, 0, sizeof(*st));
	GUEST_SYNC(0);

	check_status(st);
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	version = READ_ONCE(st->version);
	check_status(st);
	GUEST_SYNC(1);

	check_status(st);
	GUEST_ASSERT(version < READ_ONCE(st->version));
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	check_status(st);
	GUEST_DONE();
}

static bool is_steal_time_supported(struct kvm_vcpu *vcpu)
{
	return kvm_cpu_has(X86_FEATURE_KVM_STEAL_TIME);
}

static void steal_time_init(struct kvm_vcpu *vcpu, u32 i)
{
	/* ST_GPA_BASE is identity mapped */
	WRITE_AND_SYNC_TO_GUEST(vcpu->vm, st_gva[i],
				(void *)(ST_GPA_BASE + i * STEAL_TIME_SIZE));

	vcpu_set_msr(vcpu, MSR_KVM_STEAL_TIME, (unsigned long)st_gva[i] | KVM_MSR_ENABLED);
}

static void steal_time_dump(struct kvm_vm *vm, u32 vcpu_idx)
{
	struct kvm_steal_time *st = addr_gva2hva(vm, (unsigned long)st_gva[vcpu_idx]);

	ksft_print_msg("VCPU%d:\n", vcpu_idx);
	ksft_print_msg("    steal:     %lld\n", st->steal);
	ksft_print_msg("    version:   %d\n", st->version);
	ksft_print_msg("    flags:     %d\n", st->flags);
	ksft_print_msg("    preempted: %d\n", st->preempted);
	ksft_print_msg("    u8_pad:    %d %d %d\n",
			st->u8_pad[0], st->u8_pad[1], st->u8_pad[2]);
	ksft_print_msg("    pad:       %d %d %d %d %d %d %d %d %d %d %d\n",
			st->pad[0], st->pad[1], st->pad[2], st->pad[3],
			st->pad[4], st->pad[5], st->pad[6], st->pad[7],
			st->pad[8], st->pad[9], st->pad[10]);
}

static void check_steal_time_uapi(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	ret = _vcpu_set_msr(vcpu, MSR_KVM_STEAL_TIME,
			    (unsigned long)ST_GPA_BASE | KVM_STEAL_RESERVED_MASK);
	TEST_ASSERT(ret == 0, "Bad GPA didn't fail");

	kvm_vm_free(vm);
}

#elif defined(__aarch64__)

/* PV_TIME_ST must have 64-byte alignment */
#define STEAL_TIME_SIZE		((sizeof(struct st_time) + 63) & ~63)
#define NR_LIFECYCLE_TESTS	0

static void check_steal_time_lifecycle(void)
{
}

#define SMCCC_ARCH_FEATURES	0x80000001
#define PV_TIME_FEATURES	0xc5000020
#define PV_TIME_ST		0xc5000021

struct st_time {
	u32 rev;
	u32 attr;
	u64 st_time;
};

static s64 smccc(u32 func, u64 arg)
{
	struct arm_smccc_res res;

	do_smccc(func, arg, 0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static void check_status(struct st_time *st)
{
	GUEST_ASSERT_EQ(READ_ONCE(st->rev), 0);
	GUEST_ASSERT_EQ(READ_ONCE(st->attr), 0);
}

static void guest_code(int cpu)
{
	struct st_time *st;
	s64 status;

	status = smccc(SMCCC_ARCH_FEATURES, PV_TIME_FEATURES);
	GUEST_ASSERT_EQ(status, 0);
	status = smccc(PV_TIME_FEATURES, PV_TIME_FEATURES);
	GUEST_ASSERT_EQ(status, 0);
	status = smccc(PV_TIME_FEATURES, PV_TIME_ST);
	GUEST_ASSERT_EQ(status, 0);

	status = smccc(PV_TIME_ST, 0);
	GUEST_ASSERT_NE(status, -1);
	GUEST_ASSERT_EQ(status, (unsigned long)st_gva[cpu]);

	st = (struct st_time *)status;
	GUEST_SYNC(0);

	check_status(st);
	WRITE_ONCE(guest_stolen_time[cpu], st->st_time);
	GUEST_SYNC(1);

	check_status(st);
	WRITE_ONCE(guest_stolen_time[cpu], st->st_time);
	GUEST_DONE();
}

static bool is_steal_time_supported(struct kvm_vcpu *vcpu)
{
	struct kvm_device_attr dev = {
		.group = KVM_ARM_VCPU_PVTIME_CTRL,
		.attr = KVM_ARM_VCPU_PVTIME_IPA,
	};

	return !__vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &dev);
}

static void steal_time_init(struct kvm_vcpu *vcpu, u32 i)
{
	struct kvm_vm *vm = vcpu->vm;
	u64 st_ipa;

	struct kvm_device_attr dev = {
		.group = KVM_ARM_VCPU_PVTIME_CTRL,
		.attr = KVM_ARM_VCPU_PVTIME_IPA,
		.addr = (u64)&st_ipa,
	};

	/* ST_GPA_BASE is identity mapped */
	WRITE_AND_SYNC_TO_GUEST(vm, st_gva[i], (void *)(ST_GPA_BASE + i * STEAL_TIME_SIZE));

	st_ipa = (unsigned long)st_gva[i];
	vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &dev);
}

static void steal_time_dump(struct kvm_vm *vm, u32 vcpu_idx)
{
	struct st_time *st = addr_gva2hva(vm, (unsigned long)st_gva[vcpu_idx]);

	ksft_print_msg("VCPU%d:\n", vcpu_idx);
	ksft_print_msg("    rev:     %d\n", st->rev);
	ksft_print_msg("    attr:    %d\n", st->attr);
	ksft_print_msg("    st_time: %ld\n", st->st_time);
}

static void check_steal_time_uapi(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	u64 st_ipa;
	int ret;

	struct kvm_device_attr dev = {
		.group = KVM_ARM_VCPU_PVTIME_CTRL,
		.attr = KVM_ARM_VCPU_PVTIME_IPA,
		.addr = (u64)&st_ipa,
	};

	vm = vm_create_steal_time(1, NULL, &vcpu);
	vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &dev);

	st_ipa = (unsigned long)ST_GPA_BASE | 1;
	ret = __vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &dev);
	TEST_ASSERT(ret == -1 && errno == EINVAL, "Bad IPA didn't report EINVAL");

	st_ipa = (unsigned long)ST_GPA_BASE;
	vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &dev);

	ret = __vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &dev);
	TEST_ASSERT(ret == -1 && errno == EEXIST, "Set IPA twice without EEXIST");

	kvm_vm_free(vm);
}

#elif defined(__riscv)

/* SBI STA shmem must have 64-byte alignment */
#define STEAL_TIME_SIZE		((sizeof(struct sta_struct) + 63) & ~63)
#define NR_LIFECYCLE_TESTS	3

static gpa_t st_gpa[NR_VCPUS];

struct sta_struct {
	u32 sequence;
	u32 flags;
	u64 steal;
	u8 preempted;
	u8 pad[47];
} __packed;

static void sta_set_shmem(gpa_t gpa, unsigned long flags)
{
	unsigned long lo = (unsigned long)gpa;
#if __riscv_xlen == 32
	unsigned long hi = (unsigned long)(gpa >> 32);
#else
	unsigned long hi = gpa == -1 ? -1 : 0;
#endif
	struct sbiret ret = sbi_ecall(SBI_EXT_STA, 0, lo, hi, flags, 0, 0, 0);

	GUEST_ASSERT(ret.value == 0 && ret.error == 0);
}

static void check_status(struct sta_struct *st)
{
	GUEST_ASSERT(!(READ_ONCE(st->sequence) & 1));
	GUEST_ASSERT(READ_ONCE(st->flags) == 0);
	GUEST_ASSERT(READ_ONCE(st->preempted) == 0);
}

static void guest_code(int cpu)
{
	struct sta_struct *st = st_gva[cpu];
	u32 sequence;
	long out_val = 0;
	bool probe;

	probe = guest_sbi_probe_extension(SBI_EXT_STA, &out_val);
	GUEST_ASSERT(probe && out_val == 1);

	sta_set_shmem(st_gpa[cpu], 0);
	GUEST_SYNC(0);

	check_status(st);
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	sequence = READ_ONCE(st->sequence);
	check_status(st);
	GUEST_SYNC(1);

	check_status(st);
	GUEST_ASSERT(sequence < READ_ONCE(st->sequence));
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	check_status(st);
	GUEST_DONE();
}

static bool is_steal_time_supported(struct kvm_vcpu *vcpu)
{
	u64 id = RISCV_SBI_EXT_REG(KVM_RISCV_SBI_EXT_STA);
	unsigned long enabled = vcpu_get_reg(vcpu, id);

	TEST_ASSERT(enabled == 0 || enabled == 1, "Expected boolean result");

	return enabled;
}

static void steal_time_init(struct kvm_vcpu *vcpu, u32 i)
{
	/* ST_GPA_BASE is identity mapped */
	WRITE_AND_SYNC_TO_GUEST(vcpu->vm, st_gva[i], (void *)(ST_GPA_BASE + i * STEAL_TIME_SIZE));
	WRITE_AND_SYNC_TO_GUEST(vcpu->vm, st_gpa[i], addr_gva2gpa(vcpu->vm, (gva_t)st_gva[i]));
}

static void steal_time_dump(struct kvm_vm *vm, u32 vcpu_idx)
{
	struct sta_struct *st = addr_gva2hva(vm, (unsigned long)st_gva[vcpu_idx]);
	int i;

	pr_info("VCPU%d:\n", vcpu_idx);
	pr_info("    sequence:  %d\n", st->sequence);
	pr_info("    flags:     %d\n", st->flags);
	pr_info("    steal:     %"PRIu64"\n", st->steal);
	pr_info("    preempted: %d\n", st->preempted);
	pr_info("    pad:      ");
	for (i = 0; i < 47; ++i)
		pr_info("%d", st->pad[i]);
	pr_info("\n");
}

static gpa_t lifecycle_second_gpa;

static void sta_lifecycle_guest(void)
{
	sta_set_shmem(ST_GPA_BASE, 0);
	GUEST_SYNC(0);
	GUEST_SYNC(1);

	sta_set_shmem(INVALID_GPA, 0);
	GUEST_SYNC(2);
	GUEST_SYNC(3);

	/* Re-enable the same area, as the guest CPU online callback does. */
	sta_set_shmem(ST_GPA_BASE, 0);
	GUEST_SYNC(4);
	GUEST_SYNC(5);

	/* Replace the active registration without disabling it first. */
	sta_set_shmem(lifecycle_second_gpa, 0);
	GUEST_SYNC(6);
	GUEST_SYNC(7);
	sta_set_shmem(INVALID_GPA, 0);
	GUEST_DONE();
}

static void sta_lifecycle_run(struct kvm_vcpu *vcpu, unsigned int stage)
{
	struct ucall uc;

	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
		TEST_ASSERT_EQ(uc.args[1], stage);
		break;
	case UCALL_DONE:
		TEST_ASSERT_EQ(stage, 8);
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_FAIL("Unexpected STA lifecycle exit at stage %u", stage);
	}
}

static void check_sta_page(struct sta_struct *st, struct sta_struct *before)
{
	TEST_ASSERT(st->sequence != before->sequence && !(st->sequence & 1),
		    "Expected a completed STA update, sequence=%u", st->sequence);
	TEST_ASSERT(st->steal > before->steal, "STA stolen time did not advance");
	TEST_ASSERT_EQ(st->flags, 0);
	TEST_ASSERT_EQ(st->preempted, 0);
}

static void check_steal_time_lifecycle(void)
{
	struct sta_struct snapshot, before, *first, *second;
	size_t host_page_size = getpagesize();
	unsigned long dirty = 0;
	unsigned int gpages;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	vm = vm_create_with_one_vcpu(&vcpu, sta_lifecycle_guest);
	gpages = vm_calc_num_guest_pages(VM_MODE_DEFAULT, 2 * host_page_size);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, ST_GPA_BASE,
				    1, gpages, KVM_MEM_LOG_DIRTY_PAGES);
	/* Also exercise an STA structure ending at a host page boundary. */
	WRITE_AND_SYNC_TO_GUEST(vm, lifecycle_second_gpa,
				ST_GPA_BASE + 2 * host_page_size - sizeof(struct sta_struct));
	first = addr_gpa2hva(vm, ST_GPA_BASE);
	second = addr_gpa2hva(vm, lifecycle_second_gpa);

	/* Establish active updates on the first page before testing disable. */
	sta_lifecycle_run(vcpu, 0);
	before = *first;
	/* Registration writes zeroes and dirties the page independently. */
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	force_steal_time();
	sta_lifecycle_run(vcpu, 1);
	check_sta_page(first, &before);
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	TEST_ASSERT_EQ(dirty, BIT(0));

	sta_lifecycle_run(vcpu, 2);
	snapshot = *first;
	/* Entry before the disable call may have updated the old area. */
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	force_steal_time();
	sta_lifecycle_run(vcpu, 3);
	TEST_ASSERT(!memcmp(first, &snapshot, sizeof(snapshot)),
		    "Disabled STA shared page was modified");
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	TEST_ASSERT_EQ(dirty, 0);
	ksft_test_result_pass("STA disable stops shared page updates\n");

	sta_lifecycle_run(vcpu, 4);
	before = *first;
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	force_steal_time();
	sta_lifecycle_run(vcpu, 5);
	check_sta_page(first, &before);
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	TEST_ASSERT_EQ(dirty, BIT(0));
	ksft_test_result_pass("STA re-enable resumes shared page updates\n");

	sta_lifecycle_run(vcpu, 6);
	/* Snapshot after the last entry that could still update the old area. */
	snapshot = *first;
	before = *second;
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	force_steal_time();
	sta_lifecycle_run(vcpu, 7);
	check_sta_page(second, &before);
	TEST_ASSERT(!memcmp(first, &snapshot, sizeof(snapshot)),
		    "Old STA shared page was modified after replacement");
	kvm_vm_get_dirty_log(vm, 1, &dirty);
	TEST_ASSERT_EQ(dirty, BIT(1));
	ksft_test_result_pass("STA active replacement updates only the new page\n");

	sta_lifecycle_run(vcpu, 8);
	kvm_vm_free(vm);
}

static void check_steal_time_uapi(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct kvm_one_reg reg;
	u64 shmem;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	reg.id = KVM_REG_RISCV |
			 KVM_REG_SIZE_ULONG |
			 KVM_REG_RISCV_SBI_STATE |
			 KVM_REG_RISCV_SBI_STA |
			 KVM_REG_RISCV_SBI_STA_REG(shmem_lo);
	reg.addr = (u64)&shmem;

	shmem = ST_GPA_BASE + 1;
	ret = __vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
	TEST_ASSERT(ret == -1 && errno == EINVAL,
		    "misaligned STA shmem returns -EINVAL");

	shmem = ST_GPA_BASE;
	ret = __vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
	TEST_ASSERT(ret == 0,
		    "aligned STA shmem succeeds");

	shmem = INVALID_GPA;
	ret = __vcpu_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
	TEST_ASSERT(ret == 0,
		    "all-ones for STA shmem succeeds");

	kvm_vm_free(vm);
}

#elif defined(__loongarch__)

/* steal_time must have 64-byte alignment */
#define STEAL_TIME_SIZE		((sizeof(struct kvm_steal_time) + 63) & ~63)
#define NR_LIFECYCLE_TESTS	0
#define KVM_STEAL_PHYS_VALID	BIT_ULL(0)

static void check_steal_time_lifecycle(void)
{
}

struct kvm_steal_time {
	__u64 steal;
	__u32 version;
	__u32 flags;
	__u8  preempted;
	__u8  pad[47];
};

static void check_status(struct kvm_steal_time *st)
{
	GUEST_ASSERT(!(READ_ONCE(st->version) & 1));
	GUEST_ASSERT_EQ(READ_ONCE(st->flags), 0);
	GUEST_ASSERT_EQ(READ_ONCE(st->preempted), 0);
}

static void guest_code(int cpu)
{
	u32 version;
	struct kvm_steal_time *st = st_gva[cpu];

	memset(st, 0, sizeof(*st));
	GUEST_SYNC(0);

	check_status(st);
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	version = READ_ONCE(st->version);
	check_status(st);
	GUEST_SYNC(1);

	check_status(st);
	GUEST_ASSERT(version < READ_ONCE(st->version));
	WRITE_ONCE(guest_stolen_time[cpu], st->steal);
	check_status(st);
	GUEST_DONE();
}

static bool is_steal_time_supported(struct kvm_vcpu *vcpu)
{
	int err;
	u64 val;
	struct kvm_device_attr attr = {
		.group = KVM_LOONGARCH_VCPU_CPUCFG,
		.attr = CPUCFG_KVM_FEATURE,
		.addr = (u64)&val,
	};

	err = __vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &attr);
	if (err)
		return false;

	err = __vcpu_ioctl(vcpu, KVM_GET_DEVICE_ATTR, &attr);
	if (err)
		return false;

	return val & BIT(KVM_FEATURE_STEAL_TIME);
}

static void steal_time_init(struct kvm_vcpu *vcpu, u32 i)
{
	int err;
	u64 st_gpa;
	struct kvm_vm *vm = vcpu->vm;
	struct kvm_device_attr attr = {
		.group = KVM_LOONGARCH_VCPU_PVTIME_CTRL,
		.attr = KVM_LOONGARCH_VCPU_PVTIME_GPA,
		.addr = (u64)&st_gpa,
	};

	/* ST_GPA_BASE is identity mapped */
	WRITE_AND_SYNC_TO_GUEST(vm, st_gva[i], (void *)(ST_GPA_BASE + i * STEAL_TIME_SIZE));

	err = __vcpu_ioctl(vcpu, KVM_HAS_DEVICE_ATTR, &attr);
	TEST_ASSERT(err == 0, "No PV stealtime Feature");

	st_gpa = (unsigned long)st_gva[i] | KVM_STEAL_PHYS_VALID;
	err = __vcpu_ioctl(vcpu, KVM_SET_DEVICE_ATTR, &attr);
	TEST_ASSERT(err == 0, "Fail to set PV stealtime GPA");
}

static void steal_time_dump(struct kvm_vm *vm, u32 vcpu_idx)
{
	struct kvm_steal_time *st = addr_gva2hva(vm, (unsigned long)st_gva[vcpu_idx]);

	ksft_print_msg("VCPU%d:\n", vcpu_idx);
	ksft_print_msg("    steal:     %lld\n", st->steal);
	ksft_print_msg("    flags:     %d\n", st->flags);
	ksft_print_msg("    version:   %d\n", st->version);
	ksft_print_msg("    preempted: %d\n", st->preempted);
}

static void check_steal_time_uapi(void)
{

}
#endif

struct steal_time_worker {
	bool stop;
	struct timespec deadline;
};

static void *do_steal_time(void *arg)
{
	struct steal_time_worker *worker = arg;
	struct timespec ts;

	while (!READ_ONCE(worker->stop)) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (timespec_to_ns(timespec_sub(ts, worker->deadline)) >= 0)
			break;
	}

	return NULL;
}

/* The caller is pinned; its worker inherits the same CPU affinity. */
static long force_steal_time(void)
{
	struct steal_time_worker worker = {};
	struct timespec ts;
	pthread_t thread;
	cpu_set_t cpuset;
	long start, delay;
	int cpu = sched_getcpu();

	clock_gettime(CLOCK_MONOTONIC, &ts);
	worker.deadline = timespec_add_ns(ts, 5 * NSEC_PER_SEC);
	start = get_run_delay();
	kvm_pthread_create(&thread, NULL, do_steal_time, &worker);
	kvm_pthread_getaffinity(thread, &cpuset);
	TEST_ASSERT(CPU_COUNT(&cpuset) == 1 && CPU_ISSET(cpu, &cpuset),
		    "Worker failed to inherit parent's CPU affinity");

	do {
		sched_yield();
		delay = get_run_delay() - start;
		clock_gettime(CLOCK_MONOTONIC, &ts);
	} while (delay < MIN_RUN_DELAY_NS &&
		 timespec_to_ns(timespec_sub(ts, worker.deadline)) < 0);

	WRITE_ONCE(worker.stop, true);
	kvm_pthread_join(thread, NULL);
	TEST_ASSERT(delay >= MIN_RUN_DELAY_NS,
		    "Timed out waiting for runnable delay: expected >= %lu ns, got %ld",
		    MIN_RUN_DELAY_NS, delay);
	return delay;
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

static struct kvm_vm *vm_create_steal_time(u32 nr_vcpus, void *guest_code,
					   struct kvm_vcpu *vcpus[])
{
	unsigned int gpages;
	struct kvm_vm *vm;

	/* Create a VM and an identity mapped memslot for the steal time structure */
	vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
	gpages = vm_calc_num_guest_pages(VM_MODE_DEFAULT, STEAL_TIME_SIZE * nr_vcpus);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, ST_GPA_BASE,
				    1, gpages, KVM_MEM_LOG_DIRTY_PAGES);
	virt_map(vm, ST_GPA_BASE, ST_GPA_BASE, gpages);

	return vm;
}

int main(int ac, char **av)
{
	struct kvm_vcpu *vcpus[NR_VCPUS];
	unsigned long *dirty_log;
	unsigned int dirty_npages, page;
	struct kvm_vm *vm;
	long stolen_time;
	long run_delay;
	bool verbose;
	int i;

	verbose = ac > 1 && (!strncmp(av[1], "-v", 3) || !strncmp(av[1], "--verbose", 10));

	/* Set CPU affinity so we can force preemption of the VCPU */
	pin_self_to_any_cpu();

	vm = vm_create_steal_time(NR_VCPUS, guest_code, vcpus);

	ksft_print_header();
	TEST_REQUIRE(is_steal_time_supported(vcpus[0]));
	TEST_REQUIRE(!access("/proc/self/schedstat", R_OK));
	ksft_set_plan(2 * NR_VCPUS + NR_LIFECYCLE_TESTS);
	dirty_npages = memslot2region(vm, 1)->region.memory_size / getpagesize();
	dirty_log = bitmap_zalloc(dirty_npages);
	TEST_ASSERT(dirty_log, "Failed to allocate dirty log");

	check_steal_time_uapi();
	check_steal_time_lifecycle();

	/* Run test on each VCPU */
	for (i = 0; i < NR_VCPUS; ++i) {
		steal_time_init(vcpus[i], i);

		vcpu_args_set(vcpus[i], 1, i);

		/* First VCPU run initializes steal-time */
		run_vcpu(vcpus[i]);
		/* Exclude initialization writes from the accounting dirty log. */
		kvm_vm_get_dirty_log(vm, 1, dirty_log);
		force_steal_time();

		/* Second VCPU run, expect guest stolen time to be <= run_delay */
		run_vcpu(vcpus[i]);
		kvm_vm_get_dirty_log(vm, 1, dirty_log);
		for (page = 0; page < dirty_npages; page++)
			TEST_ASSERT_EQ(!!test_bit(page, dirty_log),
				       page == i * STEAL_TIME_SIZE / getpagesize());
		ksft_test_result_pass("vcpu%d shared page update is marked dirty\n", i);
		stolen_time = SYNC_FROM_GUEST_AND_READ(vm, guest_stolen_time[i]);
		run_delay = get_run_delay();
		TEST_ASSERT(stolen_time <= run_delay,
			    "Expected stolen time <= %ld, got %ld",
			    run_delay, stolen_time);

		run_delay = force_steal_time();

		/* Run VCPU again to confirm stolen time is consistent with run_delay */
		run_vcpu(vcpus[i]);
		stolen_time = SYNC_FROM_GUEST_AND_READ(vm, guest_stolen_time[i]) - stolen_time;
		TEST_ASSERT(stolen_time >= run_delay,
			    "Expected stolen time >= %ld, got %ld",
			    run_delay, stolen_time);

		if (verbose) {
			ksft_print_msg("VCPU%d: total-stolen-time=%ld test-stolen-time=%ld%s\n",
				       i, guest_stolen_time[i], stolen_time,
				       stolen_time == run_delay ?
				       " (BONUS: guest test-stolen-time even exactly matches test-run_delay)" : "");
			steal_time_dump(vm, i);
		}
		ksft_test_result_pass("vcpu%d\n", i);
	}
	free(dirty_log);
	kvm_vm_free(vm);

	/* Print results and exit() accordingly */
	ksft_finished();
}
