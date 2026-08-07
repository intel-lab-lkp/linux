// SPDX-License-Identifier: GPL-2.0
/*
 * Test for RISC-V KVM_SET_PMU_EVENT_FILTER.
 *
 * Verify that a VM-scoped PMU event filter installed via the
 * KVM_SET_PMU_EVENT_FILTER ioctl is enforced when a guest configures a
 * counter through the SBI PMU COUNTER_CFG_MATCH call:
 *
 *   - with no filter, events are programmable (baseline / PMU probe);
 *   - KVM_PMU_EVENT_DENY rejects the listed events;
 *   - KVM_PMU_EVENT_ALLOW admits only the listed events;
 *   - replacing the filter with an empty DENY list re-enables everything.
 *
 * The filter is checked before any perf event is created, so the test only
 * ever programs the cycle event (always supported by the host PMU) and varies
 * the filter *list* contents to exercise membership without depending on host
 * support for other events.  Counter management and SBI error reporting happen
 * in the guest; the host installs filters and checks the reported errors.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_util.h"
#include "test_util.h"
#include "processor.h"
#include "ucall_common.h"
#include "sbi.h"

/* SBI PMU hardware event indexes (type == HW == 0, so eidx == code). */
#define EV_CYCLES	SBI_PMU_HW_CPU_CYCLES		/* 1 */
#define EV_INSTR	SBI_PMU_HW_INSTRUCTIONS		/* 2 */

/* Must match KVM_PMU_EVENT_FILTER_MAX_EVENTS in arch/riscv/kvm/vm.c. */
#define MAX_EVENTS	256

static void guest_code(void)
{
	struct sbiret ret;
	unsigned long ctr;
	long err;

	for (;;) {
		/*
		 * Request the fixed cycle counter (cbase=0, cmask=1) for the
		 * cycle event.  The host installs (or clears) the filter
		 * before each entry, so the result reflects the active policy.
		 */
		ret = sbi_ecall(SBI_EXT_PMU, SBI_EXT_PMU_COUNTER_CFG_MATCH,
				0, 1, 0, EV_CYCLES, 0, 0);
		err = ret.error;
		ctr = ret.value;

		/* Release the counter on success so the next iteration reuses it. */
		if (!err)
			sbi_ecall(SBI_EXT_PMU, SBI_EXT_PMU_COUNTER_STOP,
				  ctr, 1, SBI_PMU_STOP_FLAG_RESET, 0, 0, 0);

		GUEST_SYNC1(err);
	}
}

static struct kvm_pmu_event_filter *
build_filter(__u32 action, __u32 flags, const __u64 *events, __u32 nevents)
{
	struct kvm_pmu_event_filter *f;
	size_t size = sizeof(*f) + (size_t)nevents * sizeof(__u64);

	f = calloc(1, size);
	TEST_ASSERT(f, "calloc(pmu_event_filter)");
	f->action = action;
	f->nevents = nevents;
	f->flags = flags;
	if (nevents && events)
		memcpy(f->events, events, nevents * sizeof(__u64));
	return f;
}

/* Install a filter, asserting success. */
static void set_filter(struct kvm_vm *vm, __u32 action,
		       const __u64 *events, __u32 nevents)
{
	struct kvm_pmu_event_filter *f = build_filter(action, 0, events, nevents);

	vm_ioctl(vm, KVM_SET_PMU_EVENT_FILTER, f);
	free(f);
}

/* Install a filter and return the raw ioctl result (for negative tests). */
static int try_set_filter(struct kvm_vm *vm, __u32 action, __u32 flags,
			  const __u64 *events, __u32 nevents)
{
	struct kvm_pmu_event_filter *f = build_filter(action, flags, events, nevents);
	int ret = __vm_ioctl(vm, KVM_SET_PMU_EVENT_FILTER, f);

	free(f);
	return ret;
}

/* Run the guest one step and return the cfg_match error code it reports. */
static long run_one(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	return (long)uc.args[0];
}

static void test_filter_case(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
			     __u32 action, const __u64 *events, __u32 nevents,
			     long expect, const char *desc)
{
	long err;

	set_filter(vm, action, events, nevents);
	err = run_one(vcpu);
	TEST_ASSERT_EQ(err, expect);
	pr_info("%s: err=%ld (expected %ld)\n", desc, err, expect);
}

static void test_bad_args(struct kvm_vm *vm)
{
	__u64 ev = EV_CYCLES;
	int ret;

	/* Invalid action. */
	errno = 0;
	ret = try_set_filter(vm, 2, 0, &ev, 1);
	TEST_ASSERT(ret < 0 && errno == EINVAL,
		    "invalid action should fail with EINVAL, got ret=%d errno=%d",
		    ret, errno);

	/* Non-zero flags are not supported. */
	errno = 0;
	ret = try_set_filter(vm, KVM_PMU_EVENT_ALLOW, 1, &ev, 1);
	TEST_ASSERT(ret < 0 && errno == EINVAL,
		    "non-zero flags should fail with EINVAL, got ret=%d errno=%d",
		    ret, errno);

	/* Too many events. */
	errno = 0;
	ret = try_set_filter(vm, KVM_PMU_EVENT_DENY, 0, NULL, MAX_EVENTS + 1);
	TEST_ASSERT(ret < 0 && errno == E2BIG,
		    "nevents > max should fail with E2BIG, got ret=%d errno=%d",
		    ret, errno);
}

int main(void)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	long err;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_PMU_EVENT_FILTER));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	/*
	 * Baseline / PMU probe: with no filter the cycle event must be
	 * programmable.  If it isn't, the host PMU is unusable in this
	 * environment (e.g. Sscofpmf unavailable under TCG); skip the rest.
	 */
	err = run_one(vcpu);
	if (err) {
		pr_info("PMU unavailable (baseline cfg_match err=%ld), skipping\n",
			err);
		kvm_vm_free(vm);
		exit(KSFT_SKIP);
	}

	/* DENY{cycles}: the cycle event is rejected. */
	test_filter_case(vm, vcpu, KVM_PMU_EVENT_DENY,
			 &(__u64){ EV_CYCLES }, 1,
			 SBI_ERR_NOT_SUPPORTED, "deny cycles");

	/* ALLOW{cycles}: the cycle event is admitted. */
	test_filter_case(vm, vcpu, KVM_PMU_EVENT_ALLOW,
			 &(__u64){ EV_CYCLES }, 1,
			 0, "allow cycles");

	/*
	 * ALLOW{instructions}: cycles is not in the allow list, so it is
	 * rejected.  Instructions itself is never programmed, so host support
	 * for it is irrelevant.
	 */
	test_filter_case(vm, vcpu, KVM_PMU_EVENT_ALLOW,
			 &(__u64){ EV_INSTR }, 1,
			 SBI_ERR_NOT_SUPPORTED, "cycles not in allow{instr}");

	/* Empty DENY list: nothing is denied, cycles is programmable again. */
	test_filter_case(vm, vcpu, KVM_PMU_EVENT_DENY, NULL, 0,
			 0, "clear (deny empty)");

	test_bad_args(vm);

	kvm_vm_free(vm);
	return 0;
}
