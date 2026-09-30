// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test KVM's handling of Guarded Control Stack exception state.
 */

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall.h"

#include <asm/ptrace.h>
#include <asm/sysreg.h>

#define PSTATE_IL_BIT	BIT(20)

static void require_guest_gcs(struct kvm_vcpu *vcpu)
{
	u64 pfr1 = vcpu_get_reg(vcpu,
				KVM_ARM64_SYS_REG(SYS_ID_AA64PFR1_EL1));

	TEST_REQUIRE(SYS_FIELD_GET(ID_AA64PFR1_EL1, GCS, pfr1) >=
		     ID_AA64PFR1_EL1_GCS_IMP);
}

static bool expect_exlock;
static unsigned int undef_count;
static unsigned int illegal_count;
static unsigned int gcs_count;
static u64 handler_pstate;
static u64 illegal_pstate;

static u64 expected_illegal_pc;
static enum udf_mode {
	UDF_COLLECT,	/* Initial UDF we collect PSTATE from */
	UDF_VALIDATE,	/* Secondary UDF that collects and validates */
	UDF_REPAIR,	/* UDF to clear EXLOCK */
	UDF_ELR_EL1,	/* Write to ELR_EL1 with EXLOCK */
	UDF_ELR_EL2,	/* Write to ELR_EL2 with EXLOCK */
	UDF_SPSR_EL1,	/* Write to SPSR_EL1 with EXLOCK */
	UDF_SPSR_EL2,	/* Write to SPSR_EL2 with EXLOCK */
} udf_mode;

static void guest_undef_handler(struct ex_regs *regs)
{
	u64 esr = read_sysreg(esr_el2);
	u64 val;

	undef_count++;

	/* Just skip the instruction either way. */
	GUEST_ASSERT_EQ(esr, ESR_ELx_IL);
	regs->pc += 4;

	switch (udf_mode) {
	case UDF_COLLECT:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				expect_exlock ? GCSCR_ELx_EXLOCKEN : 0);

		/*
		 * If we want the nested exception handler to change
		 * PSTATE it can't have EXLOCK set.
		 */
		write_sysreg_s(0, SYS_GCSCR_EL2);
		isb();

		/*
		 * Take another exception without returning so we can
		 * collect the EXLOCK state we're seeing here and make
		 * our return illegal.
		 */
		expected_illegal_pc = regs->pc;
		udf_mode = UDF_VALIDATE;
		asm volatile("udf #0" ::: "memory");

		/* Reenable so PSTATE.EXLOCK is checked */
		write_sysreg_s(expect_exlock ? GCSCR_ELx_EXLOCKEN : 0,
			       SYS_GCSCR_EL2);
		isb();
		break;

	case UDF_VALIDATE:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				0);

		/* Validate the EXLOCK of the original exception. */
		handler_pstate = regs->pstate;
		GUEST_ASSERT_EQ(!!(regs->pstate & PSR_EXLOCK_BIT),
				expect_exlock);

		/* Clearing PSTATE.EXLOCK triggers an illegal ERET. */
		regs->pstate &= ~PSR_EXLOCK_BIT;
		break;

	case UDF_REPAIR:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				0);

		/* Disable EXLOCK for the requesting context */
		regs->pstate &= ~PSR_EXLOCK_BIT;
		break;

	case UDF_ELR_EL1:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				expect_exlock ? GCSCR_ELx_EXLOCKEN : 0);

		asm volatile("mrs %0, elr_el1   \n\
	                      msr elr_el1, %0"
			     : "=r"(val) :: "memory");
		break;

	case UDF_ELR_EL2:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				expect_exlock ? GCSCR_ELx_EXLOCKEN : 0);

		asm volatile("mrs %0, elr_el2   \n\
	                      msr elr_el2, %0"
			     : "=r"(val) :: "memory");
		break;

	case UDF_SPSR_EL1:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				expect_exlock ? GCSCR_ELx_EXLOCKEN : 0);

		asm volatile("mrs %0, spsr_el1   \n\
			      msr spsr_el1, %0"
			     : "=r"(val) :: "memory");
		break;

	case UDF_SPSR_EL2:
		GUEST_ASSERT_EQ(read_sysreg_s(SYS_GCSCR_EL2) & GCSCR_ELx_EXLOCKEN,
				expect_exlock ? GCSCR_ELx_EXLOCKEN : 0);

		asm volatile("mrs %0, spsr_el2   \n\
			      msr spsr_el2, %0"
			     : "=r"(val) :: "memory");
		break;

	default:
		GUEST_FAIL("Invalid udf_mode");
	}
}

static void guest_illegal_handler(struct ex_regs *regs)
{
	u64 esr = read_sysreg(esr_el2);

	illegal_count++;
	illegal_pstate = regs->pstate;

	GUEST_ASSERT_EQ(ESR_ELx_EC(esr), ESR_ELx_EC_ILL);
	GUEST_ASSERT(esr & ESR_ELx_IL);
	GUEST_ASSERT(regs->pstate & PSTATE_IL_BIT);
	GUEST_ASSERT(!(regs->pstate & PSR_EXLOCK_BIT));
	GUEST_ASSERT_EQ(regs->pc, expected_illegal_pc);

	/* Disable exception locking so we can repair the return state. */
	write_sysreg_s(0, SYS_GCSCR_EL2);
	isb();

	/* We need our own EXLOCK clearing so we can repair the original */
	udf_mode = UDF_REPAIR;
	asm volatile("udf #0" ::: "memory");

	/* Disable EXLOCK for the generating context, making things legal */
	regs->pstate &= ~(PSTATE_IL_BIT | PSR_EXLOCK_BIT);
}

static void guest_gcs_handler(struct ex_regs *regs)
{
	u64 esr = read_sysreg(esr_el2);

	gcs_count++;

	GUEST_ASSERT_EQ(ESR_ELx_EC(esr), ESR_ELx_EC_GCS);
	GUEST_ASSERT_EQ(FIELD_GET(ESR_ELx_ExType_MASK, esr),
			ESR_ELx_ExType_EXLOCK);

	/* Disabling EXLOCKEN makes sysreg writes legal. */
	write_sysreg_s(0, SYS_GCSCR_EL2);
	isb();
}

static noinline void test_udf_exception(bool enable_exlock)
{
	unsigned int initial_undef = undef_count;
	unsigned int initial_illegal = illegal_count;
	unsigned int initial_gcs = gcs_count;

	expect_exlock = enable_exlock;
	write_sysreg_s(enable_exlock ? GCSCR_ELx_EXLOCKEN : 0,
		       SYS_GCSCR_EL2);
	isb();

	udf_mode = UDF_COLLECT;
	asm volatile("udf #0" ::: "memory");

	/* Leave GCS disabled even if a broken ERET skipped the ILL handler. */
	write_sysreg_s(0, SYS_GCSCR_EL2);
	isb();

	GUEST_ASSERT_EQ(undef_count, initial_undef + 2 + enable_exlock);
	GUEST_ASSERT_EQ(illegal_count, initial_illegal + enable_exlock);
	GUEST_ASSERT_EQ(!!(handler_pstate & PSR_EXLOCK_BIT), enable_exlock);
	if (enable_exlock)
		GUEST_ASSERT(illegal_pstate & PSTATE_IL_BIT);
	GUEST_ASSERT_EQ(initial_gcs, gcs_count);
}

static noinline void test_sysreg(enum udf_mode reg, bool enable_exlock)
{
	unsigned int initial_undef = undef_count;
	unsigned int initial_gcs = gcs_count;

	expect_exlock = enable_exlock;

	write_sysreg_s(enable_exlock ? GCSCR_ELx_EXLOCKEN : 0,
		       SYS_GCSCR_EL2);
	isb();

	udf_mode = reg;
	asm volatile("udf #0" ::: "memory");

	/* Ensure EXLOCK is disabled */
	write_sysreg_s(0, SYS_GCSCR_EL2);
	isb();

	GUEST_ASSERT_EQ(gcs_count, initial_gcs + expect_exlock);
	GUEST_ASSERT_EQ(undef_count, initial_undef + 1);
}

static void guest_code(void)
{
	GUEST_ASSERT_EQ(get_current_el(), 2);

	/*
	 * Try straightforwardly generating an exception without and
	 * with EXLOCK.
	 */
	test_udf_exception(false);
	test_sysreg(UDF_ELR_EL1, false);
	test_sysreg(UDF_ELR_EL2, false);
	test_sysreg(UDF_SPSR_EL1, false);
	test_sysreg(UDF_SPSR_EL2, false);

	test_udf_exception(true);
	test_sysreg(UDF_ELR_EL1, true);
	test_sysreg(UDF_ELR_EL2, true);
	test_sysreg(UDF_SPSR_EL1, true);
	test_sysreg(UDF_SPSR_EL2, true);

	GUEST_DONE();
}

static void test_exception_from_same_el(void)
{
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;

	vm = vm_create(1);
	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	require_guest_gcs(vcpu);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);
	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_ELx_EC_UNKNOWN, guest_undef_handler);
	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_ELx_EC_ILL, guest_illegal_handler);
	vm_install_sync_handler(vm, VECTOR_SYNC_CURRENT,
				ESR_ELx_EC_GCS, guest_gcs_handler);
	kvm_arch_vm_finalize_vcpus(vm);

	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	default:
		TEST_FAIL("Unhandled ucall: %ld", uc.cmd);
	}

	kvm_vm_free(vm);
}

static void test_exception_from_lower_el(void)
{
	struct kvm_vcpu_events events = {};
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	u64 pstate;

	vm = vm_create(1);
	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, NULL);
	require_guest_gcs(vcpu);
	kvm_arch_vm_finalize_vcpus(vm);

	/*
	 * Inject an EL0 data abort to vEL2 with EXLOCK enabled for
	 * vEL2.
	 */
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2),
		     vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_HCR_EL2)) |
		     HCR_EL2_TGE);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_GCSCR_EL2),
		     GCSCR_ELx_EXLOCKEN);
	vcpu_set_reg(vcpu, ARM64_CORE_REG(regs.pstate),
		     PSR_MODE_EL0t | PSR_EXLOCK_BIT);

	events.exception.ext_dabt_pending = true;
	vcpu_events_set(vcpu, &events);

	pstate = vcpu_get_reg(vcpu, ARM64_CORE_REG(regs.pstate));
	TEST_ASSERT_EQ(pstate & PSR_MODE_MASK, PSR_MODE_EL2h);
	TEST_ASSERT(!(pstate & PSR_EXLOCK_BIT),
		    "EXLOCK set on exception from a lower EL");

	kvm_vm_free(vm);
}

int main(void)
{
	TEST_REQUIRE(kvm_check_cap(KVM_CAP_ARM_EL2));
	test_exception_from_same_el();
	test_exception_from_lower_el();

	return 0;
}
