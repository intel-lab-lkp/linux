// SPDX-License-Identifier: GPL-2.0

#include "processor.h"

enum stages {
	GUEST_UPDATE,
	USERSPACE_UPDATE,
	GUEST_APXOFF,
};

enum egpr_ops {
	EGPRS_WRITE,
	EGPRS_CHECK,
};

#define for_each_egpr(reg)	for (reg = 16; reg <= 31; reg++)

/*
 * Deterministic per-stage test values for EGPRs so that guest and
 * userspace can validate state transitions.
 */
static inline unsigned long egpr_data(enum stages stage, int reg)
{
	switch (stage) {
	case GUEST_UPDATE:
		return 0xabcd + reg;
	case USERSPACE_UPDATE:
		return 0xbcde + reg;
	case GUEST_APXOFF:
		return 0xcdef + reg;
	default:
		return 0;
	}
}

/*
 * Read/write or validate EGPR values either directly via registers
 * (guest context) or via a provided buffer (userspace XSAVE).
 */
static bool handle_egprs(enum egpr_ops ops, unsigned long *egprs, enum stages stage)
{
	unsigned long data;
	int reg;

	for_each_egpr(reg) {
		data = egpr_data(stage, reg);

		if (ops == EGPRS_WRITE) {
			if (egprs)
				egprs[reg - 16] = data;
			else
				write_egpr(reg, data);
			continue;
		}

		if (ops != EGPRS_CHECK)
			return false;

		if (egprs) {
			if (egprs[reg - 16] != data)
				return false;
			continue;
		}

		if (read_egpr(reg) != data)
			return false;
	}

	return true;
}

static void write_egprs(enum stages stage)
{
	handle_egprs(EGPRS_WRITE, NULL, stage);
}

static bool validate_egprs(enum stages stage)
{
	return handle_egprs(EGPRS_CHECK, NULL, stage);
}

static void test_guest_update(void)
{
	write_egprs(GUEST_UPDATE);
	GUEST_SYNC(GUEST_UPDATE);
	GUEST_ASSERT(validate_egprs(GUEST_UPDATE));
}

static void test_userspace_update(void)
{
	/* Userspace updates EGPR state via the KVM XSAVE ABI */
	GUEST_SYNC(USERSPACE_UPDATE);
	GUEST_ASSERT(validate_egprs(USERSPACE_UPDATE));
}

static void test_guest_apxoff(void)
{
	write_egprs(GUEST_APXOFF);
	/* Disable APX to verify state is preserved */
	GUEST_ASSERT(!xsetbv_safe(0, this_cpu_supported_xcr0() & ~XFEATURE_MASK_APX));
	GUEST_SYNC(GUEST_APXOFF);
	GUEST_ASSERT(!xsetbv_safe(0, this_cpu_supported_xcr0()));
	GUEST_ASSERT(validate_egprs(GUEST_APXOFF));
}

static void guest_code(void)
{
	set_cr4(get_cr4() | X86_CR4_OSXSAVE);
	GUEST_ASSERT(!xsetbv_safe(0, this_cpu_supported_xcr0()));

	test_guest_update();
	test_userspace_update();
	test_guest_apxoff();

	GUEST_DONE();
}

#define X86_PROPERTY_XSTATE_APX_OFFSET	KVM_X86_CPU_PROPERTY(0xd, 19, EBX, 0, 31)
#define XSAVE_HDR_OFFSET		512

static inline unsigned long *xsave_egprs(void *xsave)
{
	return xsave + kvm_cpu_property(X86_PROPERTY_XSTATE_APX_OFFSET);
}

static inline void xstatebv_set(void *xsave, uint64_t mask)
{
	*(uint64_t *)(xsave + XSAVE_HDR_OFFSET) |= mask;
}

static void write_xsave_egprs(void *xsave, enum stages stage)
{
	handle_egprs(EGPRS_WRITE, xsave_egprs(xsave), stage);
	xstatebv_set(xsave, XFEATURE_MASK_APX);
}

static bool validate_xsave_egprs(void *xsave, enum stages stage)
{
	return handle_egprs(EGPRS_CHECK, xsave_egprs(xsave), stage);
}

int main(int argc, char *argv[])
{
	struct kvm_xsave *xsave;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	enum stages stage;
	struct ucall uc;
	int xsave_size;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_XSAVE));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_APX));

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	xsave_size = vm_check_cap(vcpu->vm, KVM_CAP_XSAVE2);
	TEST_ASSERT(xsave_size, "KVM_CAP_XSAVE2 not supported");
	xsave = malloc(xsave_size);
	TEST_ASSERT(xsave, "Failed to allocate XSAVE buffer");

	while (1) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_SYNC: {
			stage = uc.args[1];
			vcpu_xsave2_get(vcpu, xsave);
			if (stage == USERSPACE_UPDATE) {
				write_xsave_egprs(xsave, stage);
			} else {
				TEST_ASSERT(validate_xsave_egprs(xsave, stage),
					    "EGPR state mismatch in userspace XSAVE buffer");
			}
			vcpu_xsave_set(vcpu, xsave);
			break;
		}
		case UCALL_DONE:
			goto done;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}

done:
	free(xsave);
	kvm_vm_free(vm);
	return 0;
}
