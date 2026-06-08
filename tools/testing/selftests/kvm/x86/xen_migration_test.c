// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test Xen runstate (steal time) preservation across simulated migration.
 *
 * Verifies that the kernel correctly accounts the migration gap as
 * steal time (runnable) when runstate data is saved and restored
 * precisely, but real time elapses during the migration.
 *
 * The key insight: userspace saves the runstate with state=RUNSTATE_runnable
 * (the vCPU is not running during migration). On restore, the kernel sees
 * that kvmclock has advanced past state_entry_time, and accounts the
 * difference as time spent in the runnable state.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#include <asm/pvclock-abi.h>

#define SHINFO_GPA	0xc0000000ULL
#define RUNSTATE_GPA	(SHINFO_GPA + 0x1000)

#define RUNSTATE_running  0
#define RUNSTATE_runnable 1
#define RUNSTATE_blocked  2
#define RUNSTATE_offline  3

struct vcpu_runstate_info {
	uint32_t state;
	uint64_t state_entry_time;
	uint64_t time[4];
} __attribute__((packed));

static void guest_code(void)
{
	volatile struct vcpu_runstate_info *rs =
		(void *)(unsigned long)RUNSTATE_GPA;

	/* Report runstate times — no need to enable kvmclock MSR,
	 * the kernel writes runstate using its internal kvmclock. */
	GUEST_SYNC_ARGS(0, rs->time[RUNSTATE_runnable],
			rs->time[RUNSTATE_running], 0, 0);
}

static struct kvm_vm *create_xen_vm(struct kvm_vcpu **vcpu)
{
	struct kvm_vm *vm;
	int xen_caps;

	vm = vm_create_with_one_vcpu(vcpu, guest_code);

	xen_caps = kvm_check_cap(KVM_CAP_XEN_HVM);
	TEST_REQUIRE(xen_caps & KVM_XEN_HVM_CONFIG_SHARED_INFO);
	TEST_REQUIRE(xen_caps & KVM_XEN_HVM_CONFIG_RUNSTATE);

	/* Map pages */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    SHINFO_GPA, 1, 2, 0);
	virt_map(vm, SHINFO_GPA, SHINFO_GPA, 2);

	/* Enable Xen HVM with MSR interception (enables runstate tracking) */
	struct kvm_xen_hvm_config cfg = {
		.flags = KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL,
		.msr = 0x40000000,
	};
	vm_ioctl(vm, KVM_XEN_HVM_CONFIG, &cfg);

	/* Set shared_info */
	struct kvm_xen_hvm_attr ha = {
		.type = KVM_XEN_ATTR_TYPE_SHARED_INFO,
		.u.shared_info.gfn = SHINFO_GPA >> 12,
	};
	vm_ioctl(vm, KVM_XEN_HVM_SET_ATTR, &ha);

	/* Set runstate address */
	struct kvm_xen_vcpu_attr rs_addr = {
		.type = KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADDR,
		.u.gpa = RUNSTATE_GPA,
	};
	vcpu_ioctl(*vcpu, KVM_XEN_VCPU_SET_ATTR, &rs_addr);

	return vm;
}

int main(void)
{
	struct pvclock_vcpu_time_info pvti;
	struct kvm_xen_vcpu_attr runstate_save;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	uint64_t tsc_offset;
	int ret;

	/* === SOURCE SIDE === */
	pr_info("=== Source: create VM and run guest ===\n");
	vm = create_xen_vm(&vcpu);

	/* Run guest once to accumulate some runstate time */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);

	pr_info("  Guest sees: runnable=%" PRIu64 " running=%" PRIu64 "\n",
		uc.args[2], uc.args[3]);

	/* Save clock state */
	ret = __vcpu_ioctl(vcpu, KVM_GET_CLOCK_GUEST, &pvti);
	TEST_ASSERT(!ret, "KVM_GET_CLOCK_GUEST failed");

	/* Save TSC offset */
	tsc_offset = vcpu_get_msr(vcpu, MSR_IA32_TSC_ADJUST);

	/* Save runstate — the vCPU is now "runnable" (not running) */
	runstate_save.type = KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_DATA;
	vcpu_ioctl(vcpu, KVM_XEN_VCPU_GET_ATTR, &runstate_save);

	/*
	 * Transition to runnable state before saving — the vCPU is
	 * not running during migration.
	 */
	runstate_save.u.runstate.state = RUNSTATE_runnable;

	pr_info("  Saved runstate: running=%" PRIu64 " runnable=%" PRIu64
		" entry=%" PRIu64 "\n",
		(uint64_t)runstate_save.u.runstate.time_running,
		(uint64_t)runstate_save.u.runstate.time_runnable,
		(uint64_t)runstate_save.u.runstate.state_entry_time);

	uint64_t saved_runnable = runstate_save.u.runstate.time_runnable;

	kvm_vm_release(vm);

	/* === MIGRATION GAP === */
	pr_info("=== Simulating migration (sleeping 10ms) ===\n");
	usleep(10000);

	/* === DESTINATION SIDE === */
	pr_info("=== Destination: create new VM and restore ===\n");
	vm = create_xen_vm(&vcpu);

	/* Restore TSC offset */
	vcpu_set_msr(vcpu, MSR_IA32_TSC_ADJUST, tsc_offset);

	/* Restore clock — kvmclock will now be ~10ms ahead of the snapshot */
	vcpu_ioctl(vcpu, KVM_SET_CLOCK_GUEST, &pvti);

	/* Restore runstate exactly as saved (state=runnable) */
	runstate_save.type = KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_DATA;
	ret = __vcpu_ioctl(vcpu, KVM_XEN_VCPU_SET_ATTR, &runstate_save);
	TEST_ASSERT(!ret, "Restore runstate failed: errno %d", errno);

	/*
	 * Run the guest. When the vCPU enters vcpu_run, the kernel
	 * transitions from RUNSTATE_runnable to RUNSTATE_running.
	 * It computes: delta = kvmclock_now - state_entry_time
	 * This delta (which includes the migration gap) is added to
	 * time_runnable (steal time).
	 */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);

	uint64_t guest_runnable = uc.args[2];
	uint64_t guest_running = uc.args[3];

	pr_info("  Guest sees: runnable=%" PRIu64 " running=%" PRIu64 "\n",
		guest_runnable, guest_running);

	uint64_t steal_increase = guest_runnable - saved_runnable;
	pr_info("  Steal time increase: %" PRIu64 " ns (migration gap)\n",
		steal_increase);

	/*
	 * The steal time increase should be at least 10ms (the sleep)
	 * but not more than 5s (allowing for VM creation overhead).
	 * The actual gap is from the source's state_entry_time to the
	 * destination's kvmclock "now" at vcpu_load time.
	 */
	TEST_ASSERT(steal_increase >= 10000000ULL &&
		    steal_increase < 5000000000ULL,
		    "Steal time increase %" PRIu64 " ns not in expected range "
		    "[10ms, 5s]", steal_increase);

	kvm_vm_release(vm);
	pr_info("PASS: Migration gap correctly accounted as steal time\n");
	return 0;
}
