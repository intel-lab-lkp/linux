
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

struct pmu_common_event_ids {
	uint64_t pmceid0;
	uint64_t pmceid1;
} max_pmce, expected_pmce;

struct vpmu_vm {
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int gic_fd;
};

static struct vpmu_vm vpmu_vm;

#define FILTER_NR 10

struct test_desc {
	const char *name;
	struct kvm_pmu_event_filter filter[FILTER_NR];
};

#define __DEFINE_FILTER(base, num, act)		\
	((struct kvm_pmu_event_filter) {	\
		.base_event	= base,		\
		.nevents	= num,		\
		.action		= act,		\
	})

#define DEFINE_FILTER(base, act) __DEFINE_FILTER(base, 1, act)

static void guest_code(void)
{
	uint64_t pmceid0 = read_sysreg(pmceid0_el0);
	uint64_t pmceid1 = read_sysreg(pmceid1_el0);

	GUEST_ASSERT_EQ(expected_pmce.pmceid0, pmceid0);
	GUEST_ASSERT_EQ(expected_pmce.pmceid1, pmceid1);

	GUEST_DONE();
}

static void guest_get_pmceid(void)
{
	max_pmce.pmceid0 = read_sysreg(pmceid0_el0);
	max_pmce.pmceid1 = read_sysreg(pmceid1_el0);

	GUEST_DONE();
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (1) {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			return;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}
}

static void set_pmce(struct pmu_common_event_ids *pmce, int action, int event)
{
	int base = 0;
	uint64_t *pmceid = NULL;

	if (event >= 0x4000) {
		event -= 0x4000;
		base = 32;
	}

	if (event >= 0 && event <= 0x1F) {
		pmceid = &pmce->pmceid0;
	} else if (event >= 0x20 && event <= 0x3F) {
		event -= 0x20;
		pmceid = &pmce->pmceid1;
	} else {
		return;
	}

	event += base;
	if (action == KVM_PMU_EVENT_ALLOW)
		*pmceid |= BIT(event);
	else
		*pmceid &= ~BIT(event);
}

static void prepare_expected_pmce(struct kvm_pmu_event_filter *filter)
{
	struct pmu_common_event_ids pmce_mask = { ~0, ~0 };
	bool first_filter = true;
	int i;

	while (filter && filter->nevents != 0) {
		if (first_filter) {
			if (filter->action == KVM_PMU_EVENT_ALLOW)
				memset(&pmce_mask, 0, sizeof(pmce_mask));
			first_filter = false;
		}

		for (i = 0; i < filter->nevents; i++)
			set_pmce(&pmce_mask, filter->action,
				 filter->base_event + i);

		filter++;
	}

	expected_pmce.pmceid0 = max_pmce.pmceid0 & pmce_mask.pmceid0;
	expected_pmce.pmceid1 = max_pmce.pmceid1 & pmce_mask.pmceid1;
}

static void pmu_event_filter_init(struct kvm_pmu_event_filter *filter)
{
	while (filter && filter->nevents != 0) {
		kvm_device_attr_set(vpmu_vm.vcpu->fd,
				    KVM_ARM_VCPU_PMU_V3_CTRL,
				    KVM_ARM_VCPU_PMU_V3_FILTER,
				    filter);
		filter++;
	}
}

/* Create a VM that has one vCPU with PMUv3 configured. */
static void create_vpmu_vm_with_filter(void *guest_code,
				       struct kvm_pmu_event_filter *filter)
{
	uint64_t irq = 23;

	/* The test creates the vpmu_vm multiple times. Ensure a clean state */
	memset(&vpmu_vm, 0, sizeof(vpmu_vm));

	vpmu_vm.vm = vm_create(1);
	vpmu_vm.vcpu = vm_vcpu_add_with_vpmu(vpmu_vm.vm, 0, guest_code);
	vpmu_vm.gic_fd = vgic_v3_setup(vpmu_vm.vm, 1, 64);
	__TEST_REQUIRE(vpmu_vm.gic_fd >= 0,
		       "Failed to create vgic-v3, skipping");

	pmu_event_filter_init(filter);

	/* Initialize vPMU */
	vpmu_set_irq(vpmu_vm.vcpu, irq);
	vpmu_init(vpmu_vm.vcpu);
}

static void create_vpmu_vm(void *guest_code)
{
	create_vpmu_vm_with_filter(guest_code, NULL);
}

static void destroy_vpmu_vm(void)
{
	close(vpmu_vm.gic_fd);
	kvm_vm_free(vpmu_vm.vm);
}

static void test_invalid_filter(void)
{
	struct kvm_pmu_event_filter invalid;
	int ret;

	pr_info("Test: test_invalid_filter\n");

	memset(&vpmu_vm, 0, sizeof(vpmu_vm));

	vpmu_vm.vm = vm_create(1);
	vpmu_vm.vcpu = vm_vcpu_add_with_vpmu(vpmu_vm.vm, 0, guest_code);
	vpmu_vm.gic_fd = vgic_v3_setup(vpmu_vm.vm, 1, 64);
	__TEST_REQUIRE(vpmu_vm.gic_fd >= 0,
		       "Failed to create vgic-v3, skipping");

	/* The max event number is (1 << 16), set a range largeer than it. */
	invalid = __DEFINE_FILTER(BIT(15), BIT(15) + 1, 0);
	ret = __kvm_device_attr_set(vpmu_vm.vcpu->fd, KVM_ARM_VCPU_PMU_V3_CTRL,
				    KVM_ARM_VCPU_PMU_V3_FILTER, &invalid);
	TEST_ASSERT(ret && errno == EINVAL, "Set Invalid filter range "
		    "ret = %d, errno = %d (expected ret = -1, errno = EINVAL)",
		    ret, errno);

	/* Set the Invalid action. */
	invalid = __DEFINE_FILTER(0, 1, 3);
	ret = __kvm_device_attr_set(vpmu_vm.vcpu->fd, KVM_ARM_VCPU_PMU_V3_CTRL,
				    KVM_ARM_VCPU_PMU_V3_FILTER, &invalid);
	TEST_ASSERT(ret && errno == EINVAL, "Set Invalid filter action "
		    "ret = %d, errno = %d (expected ret = -1, errno = EINVAL)",
		    ret, errno);

	destroy_vpmu_vm();
}

static void run_test(struct test_desc *t)
{
	pr_info("Test: %s\n", t->name);

	create_vpmu_vm_with_filter(guest_code, t->filter);
	prepare_expected_pmce(t->filter);
	sync_global_to_guest(vpmu_vm.vm, expected_pmce);

	run_vcpu(vpmu_vm.vcpu);

	destroy_vpmu_vm();
}

static struct test_desc tests[] = {
	{
		.name = "without_filter",
		.filter = {
			{ 0 }
		},
	},
	{
		.name = "member_allow_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_SW_INCR, 0),
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_INST_RETIRED, 0),
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_BR_RETIRED, 0),
			{ 0 },
		},
	},
	{
		.name = "member_deny_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_SW_INCR, 1),
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_INST_RETIRED, 1),
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_BR_RETIRED, 1),
			{ 0 },
		},
	},
	{
		.name = "not_member_deny_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_SW_INCR, 1),
			{ 0 },
		},
	},
	{
		.name = "not_member_allow_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_SW_INCR, 0),
			{ 0 },
		},
	},
	{
		.name = "deny_chain_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_CHAIN, 1),
			{ 0 },
		},
	},
	{
		.name = "deny_cpu_cycles_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_CPU_CYCLES, 1),
			{ 0 },
		},
	},
	{
		.name = "cancel_filter",
		.filter = {
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_CPU_CYCLES, 0),
			DEFINE_FILTER(ARMV8_PMUV3_PERFCTR_CPU_CYCLES, 1),
		},
	},
	{
		.name = "multiple_filter",
		.filter = {
			__DEFINE_FILTER(0x0, 0x10, 0),
			__DEFINE_FILTER(0x6, 0x3, 1),
		},
	},
	{ 0 }
};

static void run_tests(void)
{
	struct test_desc *t;

	for (t = &tests[0]; t->name; t++)
		run_test(t);
}

int used_pmu_events[] = {
       ARMV8_PMUV3_PERFCTR_BR_RETIRED,
       ARMV8_PMUV3_PERFCTR_INST_RETIRED,
       ARMV8_PMUV3_PERFCTR_CHAIN,
};

static bool kvm_pmu_support_events(void)
{
	struct pmu_common_event_ids used_pmce = { 0, 0 };

	create_vpmu_vm(guest_get_pmceid);

	memset(&max_pmce, 0, sizeof(max_pmce));
	sync_global_to_guest(vpmu_vm.vm, max_pmce);
	run_vcpu(vpmu_vm.vcpu);
	sync_global_from_guest(vpmu_vm.vm, max_pmce);
	destroy_vpmu_vm();

	for (int i = 0; i < ARRAY_SIZE(used_pmu_events); i++)
		set_pmce(&used_pmce, KVM_PMU_EVENT_ALLOW, used_pmu_events[i]);

	return ((max_pmce.pmceid0 & used_pmce.pmceid0) == used_pmce.pmceid0) &&
	       ((max_pmce.pmceid1 & used_pmce.pmceid1) == used_pmce.pmceid1);
}

int main(void)
{
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ARM_PMU_V3));
	TEST_REQUIRE(kvm_pmu_support_events());

	run_tests();

	test_invalid_filter();
}
