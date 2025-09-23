// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "apic.h"
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "sev.h"

struct xapic_vcpu {
	struct kvm_vcpu *vcpu;
	bool is_x2apic;
	bool has_xavic_errata;
};

static void xapic_guest_code(void)
{
	cli();

	xapic_enable();

	while (1) {
		uint64_t val = (u64)xapic_read_reg(APIC_IRR) |
			       (u64)xapic_read_reg(APIC_IRR + 0x10) << 32;

		xapic_write_reg(APIC_ICR2, val >> 32);
		xapic_write_reg(APIC_ICR, val);
		GUEST_SYNC(val);
	}
}

#define X2APIC_RSVD_BITS_MASK  (GENMASK_ULL(31, 20) | \
				GENMASK_ULL(17, 16) | \
				GENMASK_ULL(13, 13))

static void x2apic_guest_code(void)
{
	cli();

	x2apic_enable();

	do {
		uint64_t val = x2apic_read_reg(APIC_IRR) |
			       x2apic_read_reg(APIC_IRR + 0x10) << 32;

		if (val & X2APIC_RSVD_BITS_MASK) {
			x2apic_write_reg_fault(APIC_ICR, val);
		} else {
			x2apic_write_reg(APIC_ICR, val);
			GUEST_ASSERT_EQ(x2apic_read_reg(APIC_ICR), val);
		}
		GUEST_SYNC(val);
	} while (1);
}

static void ____test_icr(struct xapic_vcpu *x, uint64_t val)
{
	struct kvm_vcpu *vcpu = x->vcpu;
	struct kvm_lapic_state xapic;
	struct ucall uc;
	uint64_t icr;

	/*
	 * Tell the guest what ICR value to write.  Use the IRR to pass info,
	 * all bits are valid and should not be modified by KVM (ignoring the
	 * fact that vectors 0-15 are technically illegal).
	 */
	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);
	*((u32 *)&xapic.regs[APIC_IRR]) = val;
	*((u32 *)&xapic.regs[APIC_IRR + 0x10]) = val >> 32;
	vcpu_ioctl(vcpu, KVM_SET_LAPIC, &xapic);

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], val);

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);
	icr = (u64)(*((u32 *)&xapic.regs[APIC_ICR])) |
	      (u64)(*((u32 *)&xapic.regs[APIC_ICR2])) << 32;
	if (!x->is_x2apic) {
		if (!x->has_xavic_errata)
			val &= (-1u | (0xffull << (32 + 24)));
	} else if (val & X2APIC_RSVD_BITS_MASK) {
		return;
	}

	if (x->has_xavic_errata)
		TEST_ASSERT_EQ(icr & ~APIC_ICR_BUSY, val & ~APIC_ICR_BUSY);
	else
		TEST_ASSERT_EQ(icr, val & ~APIC_ICR_BUSY);
}

static void __test_icr(struct xapic_vcpu *x, uint64_t val)
{
	/*
	 * The BUSY bit is reserved on both AMD and Intel, but only AMD treats
	 * it is as _must_ be zero.  Intel simply ignores the bit.  Don't test
	 * the BUSY bit for x2APIC, as there is no single correct behavior.
	 */
	if (!x->is_x2apic)
		____test_icr(x, val | APIC_ICR_BUSY);

	____test_icr(x, val & ~(u64)APIC_ICR_BUSY);
}

static void test_icr(struct xapic_vcpu *x)
{
	struct kvm_vcpu *vcpu = x->vcpu;
	uint64_t icr, i, j;

	icr = APIC_DEST_SELF | APIC_INT_ASSERT | APIC_DM_FIXED;
	for (i = 0; i <= 0xff; i++)
		__test_icr(x, icr | i);

	icr = APIC_INT_ASSERT | APIC_DM_FIXED;
	for (i = 0; i <= 0xff; i++)
		__test_icr(x, icr | i);

	/*
	 * Send all flavors of IPIs to non-existent vCPUs.  TODO: use number of
	 * vCPUs, not vcpu.id + 1.  Arbitrarily use vector 0xff.
	 */
	icr = APIC_INT_ASSERT | 0xff;
	for (i = 0; i < 0xff; i++) {
		if (i == vcpu->id)
			continue;
		for (j = 0; j < 8; j++)
			__test_icr(x, i << (32 + 24) | icr | (j << 8));
	}

	/* And again with a shorthand destination for all types of IPIs. */
	icr = APIC_DEST_ALLBUT | APIC_INT_ASSERT;
	for (i = 0; i < 8; i++)
		__test_icr(x, icr | (i << 8));

	/* And a few garbage value, just make sure it's an IRQ (blocked). */
	__test_icr(x, 0xa5a5a5a5a5a5a5a5 & ~APIC_DM_FIXED_MASK);
	__test_icr(x, 0x5a5a5a5a5a5a5a5a & ~APIC_DM_FIXED_MASK);
	__test_icr(x, -1ull & ~APIC_DM_FIXED_MASK);
}

static void __test_apic_id(struct kvm_vcpu *vcpu, uint64_t apic_base)
{
	uint32_t apic_id, expected;
	struct kvm_lapic_state xapic;

	vcpu_set_msr(vcpu, MSR_IA32_APICBASE, apic_base);

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);

	expected = apic_base & X2APIC_ENABLE ? vcpu->id : vcpu->id << 24;
	apic_id = *((u32 *)&xapic.regs[APIC_ID]);

	TEST_ASSERT(apic_id == expected,
		    "APIC_ID not set back to %s format; wanted = %x, got = %x",
		    (apic_base & X2APIC_ENABLE) ? "x2APIC" : "xAPIC",
		    expected, apic_id);
}

static inline bool is_sev_vm_type(int type)
{
	return type == KVM_X86_SEV_VM ||
		type == KVM_X86_SEV_ES_VM ||
		type == KVM_X86_SNP_VM;
}

static inline uint64_t get_sev_policy(int vm_type)
{
	switch (vm_type) {
	case KVM_X86_SEV_VM:
		return SEV_POLICY_NO_DBG;
	case KVM_X86_SEV_ES_VM:
		return SEV_POLICY_ES;
	case KVM_X86_SNP_VM:
		return snp_default_policy();
	default:
		return 0;
	}
}

/*
 * Verify that KVM switches the APIC_ID between xAPIC and x2APIC when userspace
 * stuffs MSR_IA32_APICBASE.  Setting the APIC_ID when x2APIC is enabled and
 * when the APIC transitions for DISABLED to ENABLED is architectural behavior
 * (on Intel), whereas the x2APIC => xAPIC transition behavior is KVM ABI since
 * attempted to transition from x2APIC to xAPIC without disabling the APIC is
 * architecturally disallowed.
 */
static void test_apic_id(int vm_type)
{
	const uint32_t NR_VCPUS = 3;
	struct kvm_vcpu *vcpus[NR_VCPUS];
	uint64_t apic_base;
	struct kvm_vm *vm;
	int i;
	struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = vm_type,
	};

	vm = __vm_create_with_vcpus(shape, NR_VCPUS, 0, NULL, vcpus);
	vm_enable_cap(vm, KVM_CAP_X2APIC_API, KVM_X2APIC_API_USE_32BIT_IDS);
	if (is_sev_vm(vm))
		vm_sev_launch(vm, get_sev_policy(vm_type), NULL);

	for (i = 0; i < NR_VCPUS; i++) {
		apic_base = vcpu_get_msr(vcpus[i], MSR_IA32_APICBASE);

		TEST_ASSERT(apic_base & MSR_IA32_APICBASE_ENABLE,
			    "APIC not in ENABLED state at vCPU RESET");
		TEST_ASSERT(!(apic_base & X2APIC_ENABLE),
			    "APIC not in xAPIC mode at vCPU RESET");

		__test_apic_id(vcpus[i], apic_base);
		__test_apic_id(vcpus[i], apic_base | X2APIC_ENABLE);
		__test_apic_id(vcpus[i], apic_base);
	}

	kvm_vm_free(vm);
}

static void test_x2apic_id(int vm_type)
{
	struct kvm_lapic_state lapic = {};
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int i;
	bool is_sev = is_sev_vm_type(vm_type);

	if (is_sev)
		vm = vm_sev_create_with_one_vcpu(vm_type, NULL, &vcpu);
	else
		vm = vm_create_with_one_vcpu(&vcpu, NULL);
	vcpu_set_msr(vcpu, MSR_IA32_APICBASE, MSR_IA32_APICBASE_ENABLE | X2APIC_ENABLE);
	if (is_sev)
		vm_sev_launch(vm, get_sev_policy(vm_type), NULL);

	/*
	 * Try stuffing a modified x2APIC ID, KVM should ignore the value and
	 * always return the vCPU's default/readonly x2APIC ID.
	 */
	for (i = 0; i <= 0xff; i++) {
		*(u32 *)(lapic.regs + APIC_ID) = i << 24;
		*(u32 *)(lapic.regs + APIC_SPIV) = APIC_SPIV_APIC_ENABLED;
		vcpu_ioctl(vcpu, KVM_SET_LAPIC, &lapic);

		vcpu_ioctl(vcpu, KVM_GET_LAPIC, &lapic);
		TEST_ASSERT(*((u32 *)&lapic.regs[APIC_ID]) == vcpu->id << 24,
			    "x2APIC ID should be fully readonly");
	}

	kvm_vm_free(vm);
}

void get_cmdline_args(int argc, char *argv[], int *vm_type)
{
	for (;;) {
		int opt = getopt(argc, argv, "t:");

		if (opt == -1)
			break;
		switch (opt) {
		case 't':
			*vm_type = parse_size(optarg);
			switch (*vm_type) {
			case KVM_X86_DEFAULT_VM:
				break;
			case KVM_X86_SEV_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV));
				break;
			case KVM_X86_SEV_ES_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_ES));
				break;
			case KVM_X86_SNP_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_SNP));
				break;
			default:
				TEST_ASSERT(false, "Unsupported VM type :%d",
					    *vm_type);
			}
			break;
		default:
			TEST_ASSERT(false,
				    "Usage: -t <vm type>. Default is %d.\n"
				    "Supported values:\n"
				    "0 - default\n"
				    "2 - SEV\n"
				    "3 - SEV-ES\n"
				    "4 - SNP",
				    KVM_X86_DEFAULT_VM);
		}
	}
}

int main(int argc, char *argv[])
{
	struct xapic_vcpu x = {
		.vcpu = NULL,
		.is_x2apic = true,
	};
	struct kvm_vm *vm;
	int vm_type = KVM_X86_DEFAULT_VM;
	bool is_sev;

	get_cmdline_args(argc, argv, &vm_type);
	is_sev = is_sev_vm_type(vm_type);

	if (is_sev)
		vm = vm_sev_create_with_one_vcpu(vm_type, x2apic_guest_code,
				&x.vcpu);
	else
		vm = vm_create_with_one_vcpu(&x.vcpu, x2apic_guest_code);

	if (is_sev_es_vm(vm))
		vm_install_exception_handler(vm, 29, sev_es_vc_handler);

	if (is_sev)
		vm_sev_launch(vm, get_sev_policy(vm_type), NULL);

	test_icr(&x);
	kvm_vm_free(vm);

	/*
	 * Use a second VM for the xAPIC test so that x2APIC can be hidden from
	 * the guest in order to test AVIC.  KVM disallows changing CPUID after
	 * KVM_RUN and AVIC is disabled if _any_ vCPU is allowed to use x2APIC.
	 */
	if (is_sev)
		vm = vm_sev_create_with_one_vcpu(vm_type, xapic_guest_code,
				&x.vcpu);
	else
		vm = vm_create_with_one_vcpu(&x.vcpu, xapic_guest_code);

	if (is_sev_es_vm(vm))
		vm_install_exception_handler(vm, 29, sev_es_vc_handler);

	x.is_x2apic = false;

	/*
	 * AMD's AVIC implementation is buggy (fails to clear the ICR BUSY bit),
	 * and also diverges from KVM with respect to ICR2[23:0] (KVM and Intel
	 * drops writes, AMD does not).  Account for the errata when checking
	 * that KVM reads back what was written.
	 */
	x.has_xavic_errata = host_cpu_is_amd &&
			     get_kvm_amd_param_bool("avic");

	vcpu_clear_cpuid_feature(x.vcpu, X86_FEATURE_X2APIC);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);
	if (is_sev)
		vm_sev_launch(vm, get_sev_policy(vm_type), NULL);

	test_icr(&x);
	kvm_vm_free(vm);

	test_apic_id(vm_type);
	test_x2apic_id(vm_type);
}
