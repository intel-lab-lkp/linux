// SPDX-License-Identifier: GPL-2.0
/*
 * pmu_event_filter_test - Test user limit pmu event for guest.
 *
 * Copyright (c) 2023 Red Hat, Inc.
 *
 * This test checks if the guest only see the limited pmu event that userspace
 * sets, if the guest can use those events which user allow, and if the guest
 * can't use those events which user deny.
 * It also checks that setting invalid filter ranges return the expected error.
 * This test runs only when KVM_CAP_ARM_PMU_V3, KVM_ARM_VCPU_PMU_V3_FILTER
 * is supported on the host.
 */
#include <kvm_util.h>
#include <processor.h>
#include <vgic.h>
#include <vpmu.h>
#include <test_util.h>
#include <perf/arm_pmuv3.h>

struct {
	uint64_t branches_retired;
	uint64_t instructions_retired;
} pmc_results;

static struct vpmu_vm *vpmu_vm;
static uint64_t pmceid0;

#define FILTER_NR 10

struct test_desc {
	const char *name;
	void (*check_result)(void);
	struct kvm_pmu_event_filter filter[FILTER_NR];
};

#define __DEFINE_FILTER(base, num, act)		\
	((struct kvm_pmu_event_filter) {	\
		.base_event	= base,		\
		.nevents	= num,		\
		.action		= act,		\
	})

#define DEFINE_FILTER(base, act) __DEFINE_FILTER(base, 1, act)

#define EMPTY_FILTER	{ 0 }

#define SW_INCR		0x0
#define INST_RETIRED	0x8
#define BR_RETIRED	0x21

#define NUM_BRANCHES	10

static void run_and_measure_loop(void)
{
	asm volatile(
		"	mov	x10, %[loop]\n"
		"1:	sub	x10, x10, #1\n"
		"	cmp	x10, #0x0\n"
		"	b.gt	1b\n"
		:
		: [loop] "r" (NUM_BRANCHES)
		: "x10", "cc");
}

static void guest_code(void)
{
	uint64_t pmcr = read_sysreg(pmcr_el0);

	pmu_disable_reset();

	write_pmevtypern(0, BR_RETIRED);
	write_pmevtypern(1, INST_RETIRED);
	enable_counter(0);
	enable_counter(1);
	write_sysreg(pmcr | ARMV8_PMU_PMCR_E, pmcr_el0);

	run_and_measure_loop();

	write_sysreg(pmcr, pmcr_el0);

	pmc_results.branches_retired = read_sysreg(pmevcntr0_el0);
	pmc_results.instructions_retired = read_sysreg(pmevcntr1_el0);

	GUEST_DONE();
}

static void guest_get_pmceid0(void)
{
	uint64_t pmceid0 = read_sysreg(pmceid0_el0);

	GUEST_PRINTF("%lx\n", pmceid0);

	GUEST_DONE();
}

static void pmu_event_filter_init(struct vpmu_vm *vm, void *arg)
{
	struct kvm_device_attr attr = {
		.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
		.attr	= KVM_ARM_VCPU_PMU_V3_FILTER,
	};
	struct kvm_pmu_event_filter *filter = (struct kvm_pmu_event_filter *)arg;

	while (filter && filter->nevents != 0) {
		attr.addr = (uint64_t)filter;
		vcpu_ioctl(vm->vcpu, KVM_SET_DEVICE_ATTR, &attr);
		filter++;
	}
}

static void create_vpmu_vm_with_filter(void *guest_code,
				       struct kvm_pmu_event_filter *filter)
{
	vpmu_vm = __create_vpmu_vm(guest_code, pmu_event_filter_init, filter);
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (1) {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			return;
		case UCALL_PRINTF:
			pmceid0 = strtoll(uc.buffer, NULL, 16);
			break;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}
}

static void check_pmc_counting(void)
{
	uint64_t br = pmc_results.branches_retired;
	uint64_t ir = pmc_results.instructions_retired;

	TEST_ASSERT(br && br == NUM_BRANCHES, "Branch instructions retired = "
		    "%lu (expected %u)", br, NUM_BRANCHES);
	TEST_ASSERT(ir, "Instructions retired = %lu (expected > 0)", ir);
}

static void check_pmc_not_counting(void)
{
	uint64_t br = pmc_results.branches_retired;
	uint64_t ir = pmc_results.instructions_retired;

	TEST_ASSERT(!br, "Branch instructions retired = %lu (expected 0)", br);
	TEST_ASSERT(!ir, "Instructions retired = %lu (expected 0)", ir);
}

static void run_vcpu_and_sync_pmc_results(void)
{
	memset(&pmc_results, 0, sizeof(pmc_results));
	sync_global_to_guest(vpmu_vm->vm, pmc_results);

	run_vcpu(vpmu_vm->vcpu);

	sync_global_from_guest(vpmu_vm->vm, pmc_results);
}

static void run_test(struct test_desc *t)
{
	pr_debug("Test: %s\n", t->name);

	create_vpmu_vm_with_filter(guest_code, t->filter);

	run_vcpu_and_sync_pmc_results();

	t->check_result();

	destroy_vpmu_vm(vpmu_vm);
}

static struct test_desc tests[] = {
	{"without_filter", check_pmc_counting, { EMPTY_FILTER }},
	{"member_allow_filter", check_pmc_counting,
	 {DEFINE_FILTER(SW_INCR, 0), DEFINE_FILTER(INST_RETIRED, 0),
	  DEFINE_FILTER(BR_RETIRED, 0), EMPTY_FILTER}},
	{"member_deny_filter", check_pmc_not_counting,
	 {DEFINE_FILTER(SW_INCR, 1), DEFINE_FILTER(INST_RETIRED, 1),
	  DEFINE_FILTER(BR_RETIRED, 1), EMPTY_FILTER}},
	{"not_member_deny_filter", check_pmc_counting,
	 {DEFINE_FILTER(SW_INCR, 1), EMPTY_FILTER}},
	{"not_member_allow_filter", check_pmc_not_counting,
	 {DEFINE_FILTER(SW_INCR, 0), EMPTY_FILTER}},
	{ 0 }
};

static void for_each_test(void)
{
	struct test_desc *t;

	for (t = &tests[0]; t->name; t++)
		run_test(t);
}

static void set_invalid_filter(struct vpmu_vm *vm, void *arg)
{
	struct kvm_pmu_event_filter invalid;
	struct kvm_device_attr attr = {
		.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
		.attr	= KVM_ARM_VCPU_PMU_V3_FILTER,
		.addr	= (uint64_t)&invalid,
	};
	int ret = 0;

	/* The max event number is (1 << 16), set a range large than it. */
	invalid = __DEFINE_FILTER(BIT(15), BIT(15)+1, 0);
	ret = __vcpu_ioctl(vm->vcpu, KVM_SET_DEVICE_ATTR, &attr);
	TEST_ASSERT(ret && errno == EINVAL, "Set Invalid filter range "
		    "ret = %d, errno = %d (expected ret = -1, errno = EINVAL)",
		    ret, errno);

	ret = 0;

	/* Set the Invalid action. */
	invalid = __DEFINE_FILTER(0, 1, 3);
	ret = __vcpu_ioctl(vm->vcpu, KVM_SET_DEVICE_ATTR, &attr);
	TEST_ASSERT(ret && errno == EINVAL, "Set Invalid filter action "
		    "ret = %d, errno = %d (expected ret = -1, errno = EINVAL)",
		    ret, errno);
}

static void test_invalid_filter(void)
{
	vpmu_vm = __create_vpmu_vm(guest_code, set_invalid_filter, NULL);
	destroy_vpmu_vm(vpmu_vm);
}

static bool kvm_supports_pmu_event_filter(void)
{
	int r;

	vpmu_vm = create_vpmu_vm(guest_code);

	r = __kvm_has_device_attr(vpmu_vm->vcpu->fd, KVM_ARM_VCPU_PMU_V3_CTRL,
				  KVM_ARM_VCPU_PMU_V3_FILTER);

	destroy_vpmu_vm(vpmu_vm);
	return !r;
}

static bool host_pmu_supports_events(void)
{
	vpmu_vm = create_vpmu_vm(guest_get_pmceid0);

	run_vcpu(vpmu_vm->vcpu);

	destroy_vpmu_vm(vpmu_vm);

	return pmceid0 & (BR_RETIRED | INST_RETIRED);
}

int main(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ARM_PMU_V3));
	TEST_REQUIRE(kvm_supports_pmu_event_filter());
	TEST_REQUIRE(host_pmu_supports_events());

	for_each_test();

	test_invalid_filter();
}
