// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "apic.h"
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

struct xapic_vcpu {
	struct kvm_vcpu *vcpu;
	bool is_x2apic;
	bool has_xavic_errata;
};

#define IRQ_VECTOR 0x20

/* See also the comment at similar assertion in memslot_perf_test.c */
static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic int is not lockless");

static atomic_uint tpr_guest_irq_sync_val;

static void tpr_guest_irq_sync_flag_reset(void)
{
	atomic_store_explicit(&tpr_guest_irq_sync_val, 0,
			      memory_order_release);
}

static unsigned int tpr_guest_irq_sync_val_get(void)
{
	return atomic_load_explicit(&tpr_guest_irq_sync_val,
				    memory_order_acquire);
}

static void tpr_guest_irq_sync_val_inc(void)
{
	atomic_fetch_add_explicit(&tpr_guest_irq_sync_val, 1,
				  memory_order_acq_rel);
}

static void tpr_guest_irq_handler_xapic(struct ex_regs *regs)
{
	tpr_guest_irq_sync_val_inc();

	xapic_write_reg(APIC_EOI, 0);
}

static void tpr_guest_irq_handler_x2apic(struct ex_regs *regs)
{
	tpr_guest_irq_sync_val_inc();

	x2apic_write_reg(APIC_EOI, 0);
}

static void tpr_guest_irq_queue(bool x2apic)
{
	if (x2apic) {
		x2apic_write_reg(APIC_SELF_IPI, IRQ_VECTOR);
	} else {
		uint32_t icr, icr2;

		icr = APIC_DEST_SELF | APIC_DEST_PHYSICAL | APIC_DM_FIXED |
			IRQ_VECTOR;
		icr2 = 0;

		xapic_write_reg(APIC_ICR2, icr2);
		xapic_write_reg(APIC_ICR, icr);
	}
}

static uint8_t tpr_guest_tpr_get(bool x2apic)
{
	uint32_t taskpri;

	if (x2apic)
		taskpri = x2apic_read_reg(APIC_TASKPRI);
	else
		taskpri = xapic_read_reg(APIC_TASKPRI);

	return (taskpri & APIC_TASKPRI_TP_MASK) >> APIC_TASKPRI_TP_SHIFT;
}

static uint8_t tpr_guest_ppr_get(bool x2apic)
{
	uint32_t procpri;

	if (x2apic)
		procpri = x2apic_read_reg(APIC_PROCPRI);
	else
		procpri = xapic_read_reg(APIC_PROCPRI);

	return (procpri & APIC_PROCPRI_PP_MASK) >> APIC_PROCPRI_PP_SHIFT;
}

static uint8_t tpr_guest_cr8_get(void)
{
	uint64_t cr8;

	asm volatile ("mov %%cr8, %[cr8]\n\t" : [cr8] "=r"(cr8));

	return cr8 & GENMASK(3, 0);
}

static void tpr_guest_check_tpr_ppr_cr8_equal(bool x2apic)
{
	uint8_t tpr;

	tpr = tpr_guest_tpr_get(x2apic);

	GUEST_ASSERT_EQ(tpr_guest_ppr_get(x2apic), tpr);
	GUEST_ASSERT_EQ(tpr_guest_cr8_get(), tpr);
}

static void tpr_guest_code(uint64_t x2apic)
{
	cli();

	if (x2apic)
		x2apic_enable();
	else
		xapic_enable();

	tpr_guest_check_tpr_ppr_cr8_equal(x2apic);

	tpr_guest_irq_queue(x2apic);

	/* TPR = 0 but IRQ masked by IF=0, should not fire */
	udelay(1000);
	GUEST_ASSERT_EQ(tpr_guest_irq_sync_val_get(), 0);

	sti();

	/* IF=1 now, IRQ should fire */
	while (tpr_guest_irq_sync_val_get() == 0)
		cpu_relax();
	GUEST_ASSERT_EQ(tpr_guest_irq_sync_val_get(), 1);

	GUEST_SYNC(0);
	tpr_guest_check_tpr_ppr_cr8_equal(x2apic);

	tpr_guest_irq_queue(x2apic);

	/* IRQ masked by barely high enough TPR now, should not fire */
	udelay(1000);
	GUEST_ASSERT_EQ(tpr_guest_irq_sync_val_get(), 1);

	GUEST_SYNC(1);
	tpr_guest_check_tpr_ppr_cr8_equal(x2apic);

	/* TPR barely low enough now to unmask IRQ, should fire */
	while (tpr_guest_irq_sync_val_get() == 1)
		cpu_relax();
	GUEST_ASSERT_EQ(tpr_guest_irq_sync_val_get(), 2);

	GUEST_DONE();
}

static uint8_t lapic_tpr_get(struct kvm_lapic_state *xapic)
{
	return (*((u32 *)&xapic->regs[APIC_TASKPRI]) & APIC_TASKPRI_TP_MASK) >>
		APIC_TASKPRI_TP_SHIFT;
}

static void lapic_tpr_set(struct kvm_lapic_state *xapic, uint8_t val)
{
	*((u32 *)&xapic->regs[APIC_TASKPRI]) &= ~APIC_TASKPRI_TP_MASK;
	*((u32 *)&xapic->regs[APIC_TASKPRI]) |= val << APIC_TASKPRI_TP_SHIFT;
}

static uint8_t sregs_tpr(struct kvm_sregs *sregs)
{
	return sregs->cr8 & GENMASK(3, 0);
}

static void test_tpr_check_tpr_zero(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic_state xapic;

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);

	TEST_ASSERT_EQ(lapic_tpr_get(&xapic), 0);
}

static void test_tpr_check_tpr_cr8_equal(struct kvm_vcpu *vcpu)
{
	struct kvm_sregs sregs;
	struct kvm_lapic_state xapic;

	vcpu_sregs_get(vcpu, &sregs);
	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);

	TEST_ASSERT_EQ(sregs_tpr(&sregs), lapic_tpr_get(&xapic));
}

static void test_tpr_mask_irq(struct kvm_vcpu *vcpu, bool mask)
{
	struct kvm_lapic_state xapic;
	uint8_t tpr;

	static_assert(IRQ_VECTOR >= 16, "invalid IRQ vector number");
	tpr = IRQ_VECTOR / 16;
	if (!mask)
		tpr--;

	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &xapic);
	lapic_tpr_set(&xapic, tpr);
	vcpu_ioctl(vcpu, KVM_SET_LAPIC, &xapic);
}

static void test_tpr(struct kvm_vcpu *vcpu, bool x2apic)
{
	bool run_guest = true;

	vcpu_args_set(vcpu, 1, (uint64_t)x2apic);

	/* According to the SDM/APM the TPR value at reset is 0 */
	test_tpr_check_tpr_zero(vcpu);
	test_tpr_check_tpr_cr8_equal(vcpu);

	tpr_guest_irq_sync_flag_reset();

	while (run_guest) {
		struct ucall uc;

		alarm(2);
		vcpu_run(vcpu);
		alarm(0);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_DONE:
			test_tpr_check_tpr_cr8_equal(vcpu);

			run_guest = false;
			break;
		case UCALL_SYNC:
			test_tpr_check_tpr_cr8_equal(vcpu);

			if (uc.args[1] == 0)
				test_tpr_mask_irq(vcpu, true);
			else if (uc.args[1] == 1)
				test_tpr_mask_irq(vcpu, false);
			else
				TEST_FAIL("Unknown SYNC %lu", uc.args[1]);
			break;
		default:
			TEST_FAIL("Unknown ucall result 0x%lx", uc.cmd);
			break;
		}
	}
}

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

/*
 * Verify that KVM switches the APIC_ID between xAPIC and x2APIC when userspace
 * stuffs MSR_IA32_APICBASE.  Setting the APIC_ID when x2APIC is enabled and
 * when the APIC transitions for DISABLED to ENABLED is architectural behavior
 * (on Intel), whereas the x2APIC => xAPIC transition behavior is KVM ABI since
 * attempted to transition from x2APIC to xAPIC without disabling the APIC is
 * architecturally disallowed.
 */
static void test_apic_id(void)
{
	const uint32_t NR_VCPUS = 3;
	struct kvm_vcpu *vcpus[NR_VCPUS];
	uint64_t apic_base;
	struct kvm_vm *vm;
	int i;

	vm = vm_create_with_vcpus(NR_VCPUS, NULL, vcpus);
	vm_enable_cap(vm, KVM_CAP_X2APIC_API, KVM_X2APIC_API_USE_32BIT_IDS);

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

static void clear_x2apic_cap_map_apic(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_X2APIC);
	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);
}

static void test_x2apic_id(void)
{
	struct kvm_lapic_state lapic = {};
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int i;

	vm = vm_create_with_one_vcpu(&vcpu, NULL);
	vcpu_set_msr(vcpu, MSR_IA32_APICBASE, MSR_IA32_APICBASE_ENABLE | X2APIC_ENABLE);

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

int main(int argc, char *argv[])
{
	struct xapic_vcpu x = {
		.vcpu = NULL,
		.is_x2apic = true,
	};
	struct kvm_vm *vm;

	/* x2APIC tests */

	vm = vm_create_with_one_vcpu(&x.vcpu, x2apic_guest_code);
	test_icr(&x);
	kvm_vm_free(vm);

	vm = vm_create_with_one_vcpu(&x.vcpu, tpr_guest_code);
	vm_install_exception_handler(vm, IRQ_VECTOR, tpr_guest_irq_handler_x2apic);
	test_tpr(x.vcpu, true);
	kvm_vm_free(vm);

	/*
	 * Use a second VM for the xAPIC test so that x2APIC can be hidden from
	 * the guest in order to test AVIC.  KVM disallows changing CPUID after
	 * KVM_RUN and AVIC is disabled if _any_ vCPU is allowed to use x2APIC.
	 */
	vm = vm_create_with_one_vcpu(&x.vcpu, xapic_guest_code);
	x.is_x2apic = false;

	/*
	 * AMD's AVIC implementation is buggy (fails to clear the ICR BUSY bit),
	 * and also diverges from KVM with respect to ICR2[23:0] (KVM and Intel
	 * drops writes, AMD does not).  Account for the errata when checking
	 * that KVM reads back what was written.
	 */
	x.has_xavic_errata = host_cpu_is_amd &&
			     get_kvm_amd_param_bool("avic");

	clear_x2apic_cap_map_apic(vm, x.vcpu);
	test_icr(&x);
	kvm_vm_free(vm);

	/* Also do a TPR non-x2APIC test */
	vm = vm_create_with_one_vcpu(&x.vcpu, tpr_guest_code);
	clear_x2apic_cap_map_apic(vm, x.vcpu);
	vm_install_exception_handler(vm, IRQ_VECTOR, tpr_guest_irq_handler_xapic);
	test_tpr(x.vcpu, false);
	kvm_vm_free(vm);

	test_apic_id();
	test_x2apic_id();
}
