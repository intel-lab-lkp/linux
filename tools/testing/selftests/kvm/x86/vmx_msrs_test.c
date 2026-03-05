// SPDX-License-Identifier: GPL-2.0-only
/*
 * VMX control MSR test
 *
 * Copyright (C) 2022 Google LLC.
 *
 * Tests for KVM ownership of bits in the VMX entry/exit control MSRs. Checks
 * that KVM will set owned bits where appropriate, and will not if
 * KVM_X86_QUIRK_TWEAK_VMX_CTRL_MSRS is disabled.
 */
#include <linux/bitmap.h>
#include "kvm_util.h"
#include "vmx.h"

static void vmx_fixed1_msr_test(struct kvm_vcpu *vcpu, uint32_t msr_index,
				  uint64_t mask)
{
	uint64_t val = vcpu_get_msr(vcpu, msr_index);
	uint64_t bit;

	mask &= val;

	for_each_set_bit(bit, &mask, 64) {
		vcpu_set_msr(vcpu, msr_index, val & ~BIT_ULL(bit));
		vcpu_set_msr(vcpu, msr_index, val);
	}
}

static void vmx_fixed0_msr_test(struct kvm_vcpu *vcpu, uint32_t msr_index,
				uint64_t mask)
{
	uint64_t val = vcpu_get_msr(vcpu, msr_index);
	uint64_t bit;

	mask = ~mask | val;

	for_each_clear_bit(bit, &mask, 64) {
		vcpu_set_msr(vcpu, msr_index, val | BIT_ULL(bit));
		vcpu_set_msr(vcpu, msr_index, val);
	}
}

static void vmx_fixed0and1_msr_test(struct kvm_vcpu *vcpu, uint32_t msr_index)
{
	vmx_fixed0_msr_test(vcpu, msr_index, GENMASK_ULL(31, 0));
	vmx_fixed1_msr_test(vcpu, msr_index, GENMASK_ULL(63, 32));
}

static void vmx_save_restore_msrs_test(struct kvm_vcpu *vcpu)
{
	union vmx_ctrl_msr ctls;
	const struct kvm_msr_list *feature_list;
	bool ctl3_found = false;
	int i;

	vcpu_set_msr(vcpu, MSR_IA32_VMX_VMCS_ENUM, 0);
	vcpu_set_msr(vcpu, MSR_IA32_VMX_VMCS_ENUM, -1ull);

	vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_BASIC,
			    BIT_ULL(49) | BIT_ULL(54) | BIT_ULL(55));

	vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_MISC,
			    BIT_ULL(5) | GENMASK_ULL(8, 6) | BIT_ULL(14) |
			    BIT_ULL(15) | BIT_ULL(28) | BIT_ULL(29) | BIT_ULL(30));

	vmx_fixed0and1_msr_test(vcpu, MSR_IA32_VMX_PROCBASED_CTLS2);
	vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_EPT_VPID_CAP, -1ull);
	vmx_fixed0and1_msr_test(vcpu, MSR_IA32_VMX_TRUE_PINBASED_CTLS);
	vmx_fixed0and1_msr_test(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	vmx_fixed0and1_msr_test(vcpu, MSR_IA32_VMX_TRUE_EXIT_CTLS);
	vmx_fixed0and1_msr_test(vcpu, MSR_IA32_VMX_TRUE_ENTRY_CTLS);
	vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_VMFUNC, -1ull);

	ctls.val = kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS);
	TEST_ASSERT(!(ctls.set & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS),
		    "CPU_BASED_ACTIVATE_TERTIARY_CONTROLS should be cleared.");

	feature_list = kvm_get_feature_msr_index_list();
	for (i = 0; i < feature_list->nmsrs; i++) {
		if (feature_list->indices[i] == MSR_IA32_VMX_PROCBASED_CTLS3) {
			ctl3_found = true;
			break;
		}
	}

	if (ctls.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS) {
		uint64_t kvm_ctls3, ctls3;

		TEST_ASSERT(ctl3_found,
			    "MSR_IA32_VMX_PROCBASED_CTLS3 was not in feature msr index list.");

		kvm_ctls3 = kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS3);
		ctls3 = vcpu_get_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3);
		TEST_ASSERT(kvm_ctls3 == ctls3,
			    "msr values for kvm and vcpu must match.");

		vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, 0);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);
		vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);

		/*
		 * The kvm host should be able to get/set
		 * MSR_IA32_VMX_PROCBASED_CTLS3 irrespective to the bit
		 * CPU_BASED_ACTIVATE_TERTIARY_CONTROLS of
		 * MSR_IA32_VMX_TRUE_PROCBASED_CTLS.
		 */
		ctls.val = vcpu_get_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS,
			     ctls.set & ~CPU_BASED_ACTIVATE_TERTIARY_CONTROLS);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, 0);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);
		vmx_fixed1_msr_test(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, ctls3);
		vcpu_set_msr(vcpu, MSR_IA32_VMX_TRUE_PROCBASED_CTLS, ctls.val);
	} else {
		TEST_ASSERT(!ctl3_found,
			    "MSR_IA32_VMX_PROCBASED_CTLS3 was in feature msr index list.");

		TEST_ASSERT(!_vcpu_set_msr(vcpu, MSR_IA32_VMX_PROCBASED_CTLS3, 0),
			    "setting MSR_IA32_VMX_PROCBASED_CTLS3 didn't fail.");
	}
}

static void __ia32_feature_control_msr_test(struct kvm_vcpu *vcpu,
					    uint64_t msr_bit,
					    struct kvm_x86_cpu_feature feature)
{
	uint64_t val;

	vcpu_clear_cpuid_feature(vcpu, feature);

	val = vcpu_get_msr(vcpu, MSR_IA32_FEAT_CTL);
	vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, val | msr_bit | FEAT_CTL_LOCKED);
	vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, (val & ~msr_bit) | FEAT_CTL_LOCKED);
	vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, val | msr_bit | FEAT_CTL_LOCKED);
	vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, (val & ~msr_bit) | FEAT_CTL_LOCKED);
	vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, val);

	if (!kvm_cpu_has(feature))
		return;

	vcpu_set_cpuid_feature(vcpu, feature);
}

static void ia32_feature_control_msr_test(struct kvm_vcpu *vcpu)
{
	uint64_t supported_bits = FEAT_CTL_LOCKED |
				  FEAT_CTL_VMX_ENABLED_INSIDE_SMX |
				  FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX |
				  FEAT_CTL_SGX_LC_ENABLED |
				  FEAT_CTL_SGX_ENABLED |
				  FEAT_CTL_LMCE_ENABLED;
	int bit, r;

	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_VMX_ENABLED_INSIDE_SMX, X86_FEATURE_SMX);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_VMX_ENABLED_INSIDE_SMX, X86_FEATURE_VMX);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX, X86_FEATURE_VMX);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_SGX_LC_ENABLED, X86_FEATURE_SGX_LC);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_SGX_LC_ENABLED, X86_FEATURE_SGX);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_SGX_ENABLED, X86_FEATURE_SGX);
	__ia32_feature_control_msr_test(vcpu, FEAT_CTL_LMCE_ENABLED, X86_FEATURE_MCE);

	for_each_clear_bit(bit, &supported_bits, 64) {
		r = _vcpu_set_msr(vcpu, MSR_IA32_FEAT_CTL, BIT(bit));
		TEST_ASSERT(r == 0,
			    "Setting reserved bit %d in IA32_FEATURE_CONTROL should fail", bit);
	}
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_DISABLE_QUIRKS2));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));

	/* No need to actually do KVM_RUN, thus no guest code. */
	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	vmx_save_restore_msrs_test(vcpu);
	ia32_feature_control_msr_test(vcpu);

	kvm_vm_free(vm);
}
