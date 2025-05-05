// SPDX-License-Identifier: GPL-2.0-only

#include <linux/acpi.h>
#include <linux/types.h>
#include <asm/acpi.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_ras.h>
#include <asm/system_misc.h>

/*
 * Was this synchronous external abort a RAS notification?
 * Returns 0 for errors handled by some RAS subsystem, or -ENOENT.
 */
static int kvm_delegate_guest_sea(void)
{
	/* apei_claim_sea(NULL) expects to mask interrupts itself. */
	lockdep_assert_irqs_enabled();
	return apei_claim_sea(NULL);
}

int kvm_handle_guest_sea(struct kvm_vcpu *vcpu)
{
	struct kvm_run *run = vcpu->run;
	bool exit = test_bit(KVM_ARCH_FLAG_RETURN_SEA_TO_USER,
			     &vcpu->kvm->arch.flags);

	/* For RAS the host kernel may handle this abort. */
	if (kvm_delegate_guest_sea() == 0)
		return 1;

	if (!exit) {
		/* Fallback behavior prior to KVM_EXIT_ARM_SEA. */
		kvm_inject_vabt(vcpu);
		return 1;
	}

	run->exit_reason = KVM_EXIT_ARM_SEA;
	run->arm_sea.esr = kvm_vcpu_get_esr(vcpu);
	run->arm_sea.flags = 0ULL;
	run->arm_sea.gva = 0ULL;
	run->arm_sea.gpa = 0ULL;

	if (kvm_vcpu_sea_far_valid(vcpu)) {
		run->arm_sea.flags |= KVM_EXIT_ARM_SEA_FLAG_GVA_VALID;
		run->arm_sea.gva = kvm_vcpu_get_hfar(vcpu);
	}

	if (kvm_vcpu_sea_ipa_valid(vcpu)) {
		run->arm_sea.flags |= KVM_EXIT_ARM_SEA_FLAG_GPA_VALID;
		run->arm_sea.gpa = kvm_vcpu_get_fault_ipa(vcpu);
	}

	return 0;
}
