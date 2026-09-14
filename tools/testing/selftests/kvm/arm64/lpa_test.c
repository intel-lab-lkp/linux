// SPDX-License-Identifier: GPL-2.0-only
/*
 * Higher virtual addresses read/write test
 */
#include <test_util.h>

#include "kvm_syscalls.h"
#include "kvm_util.h"
#include "ucall_common.h"
#include "guest_modes.h"
#include "processor.h"

#define LOW_GVA		0xc0000000
#define HIGH_GVA (LOW_GVA | BIT(48))

static void guest_code(void)
{
	u64 *low = (u64 *)LOW_GVA;
	u64 *high = (u64 *)HIGH_GVA;

	WRITE_ONCE(*low, 1);
	GUEST_ASSERT_EQ(READ_ONCE(*high), 1);
	WRITE_ONCE(*high, 2);
	GUEST_ASSERT_EQ(READ_ONCE(*low), 2);

	GUEST_SYNC(0);

	asm volatile("dsb sy\n\t"
		     "tlbi vmalle1is\n\t"
		     "dsb sy\n\t"
		     "isb"
		     : : : "memory");

	GUEST_SYNC(1); /* Host installs HIGH_GVA -> gpa_b. */

	asm volatile("dsb sy\n\t"
		     "isb\n\t"
		     : : : "memory");

	/* low and high map to different GPAs. */
	WRITE_ONCE(*low,  1);
	WRITE_ONCE(*high, 2);

	/* Check both after both writes to detect accidental aliasing. */
	GUEST_ASSERT_EQ(READ_ONCE(*low),  1);
	GUEST_ASSERT_EQ(READ_ONCE(*high), 2);

	/* Updating the low mapping must not change the high mapping. */
	WRITE_ONCE(*low, 3);

	GUEST_ASSERT_EQ(READ_ONCE(*low),  3);
	GUEST_ASSERT_EQ(READ_ONCE(*high), 2);

	GUEST_DONE();
}

static void run_test(enum vm_guest_mode mode, void *arg)
{
	unsigned int *nr_tests = arg;
	struct kvm_vcpu_init init;
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct ucall uc;

	if (mode != VM_MODE_P52V52_4K &&
	    mode != VM_MODE_P52V52_16K &&
	    mode != VM_MODE_P52V52_64K)
		return;

	vm = __vm_create(VM_SHAPE(mode), 1, 0);
	kvm_get_default_vcpu_target(vm, &init);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);

	u64 gpa_a = vm_phy_pages_alloc(vm, 1, 0, 0);
	u64 gpa_b = vm_phy_pages_alloc(vm, 1, 0, 0);

	virt_pg_map(vm, LOW_GVA, gpa_a);
	virt_pg_map(vm, HIGH_GVA, gpa_a);

	for (;;) {
		vcpu_run(vcpu);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			goto done;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_SYNC:
			switch (uc.args[1]) {
			case 0:
				WRITE_ONCE(*virt_get_pte_hva(vm, HIGH_GVA), 0);
				break;
			case 1:
				virt_pg_map(vm, HIGH_GVA, gpa_b);
				break;
			default:
				TEST_FAIL("Unexpected sync stage: %lu\n", uc.args[1]);
			}
			/* Complete the host's PTE store before resuming the guest. */
			asm volatile("dsb sy" : : : "memory");
			break;
		default:
			TEST_FAIL("Unhandled ucall: %ld\n", uc.cmd);
		}
	}

done:
	kvm_vm_free(vm);
	(*nr_tests)++;
}

int main(int argc, char **argv)
{
	unsigned int nr_tests = 0;

	for_each_guest_mode(run_test, &nr_tests);
	if (!nr_tests)
		ksft_exit_skip("No supported 52-bit VA guest mode; skipping test\n");
	return 0;
}
