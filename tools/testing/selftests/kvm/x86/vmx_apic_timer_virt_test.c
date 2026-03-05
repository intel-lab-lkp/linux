// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Intel Corporation
 *
 * Test timer expiration conversion and exercise various LVTT mode.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

#include <string.h>
#include <sys/ioctl.h>
#include <stdatomic.h>

#include <linux/math64.h>

static bool nested;

#define L2_GUEST_STACK_SIZE 256
static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

static uint64_t l2_tsc_offset;
static uint64_t l2_tsc_multiplier;
static uint64_t l2_tsc_khz;

static uint64_t host_tsc_khz;
static uint64_t max_guest_tsc_khz;

/* Any value [32, 255] for timer vector is okay. */
#define TIMER_VECTOR   0xec

static atomic_int timer_interrupted;

static void guest_timer_interrupt_handler(struct ex_regs *regs)
{
	atomic_fetch_add(&timer_interrupted, 1);
	x2apic_write_reg(APIC_EOI, 0);
}

static void reap_interrupt(void)
{
	GUEST_ASSERT(!wrmsr_safe(MSR_IA32_TSC_DEADLINE, 0));
	serialize();
	sti_nop_cli();
}

static void deadline_write_test(bool do_interrupt, bool mask,
				uint64_t deadlines[], size_t nr_deadlines)
{
	int i;

	for (i = 0; i < nr_deadlines; i++) {
		uint64_t deadline = deadlines[i];
		uint64_t val;

		reap_interrupt();

		atomic_store(&timer_interrupted, 0);
		sti();
		GUEST_ASSERT(!wrmsr_safe(MSR_IA32_TSC_DEADLINE, deadline));
		/* serialize to wait for timer interrupt to fire. */
		serialize();
		cli();

		GUEST_ASSERT(!rdmsr_safe(MSR_IA32_TSC_DEADLINE, &val));

		if (do_interrupt) {
			GUEST_ASSERT(val == 0);
			if (mask || deadline == 0)
				GUEST_ASSERT_EQ(atomic_load(&timer_interrupted), 0);
			else
				GUEST_ASSERT_EQ(atomic_load(&timer_interrupted), 1);
		} else {
			GUEST_ASSERT_EQ(val, deadline);
			GUEST_ASSERT_EQ(atomic_load(&timer_interrupted), 0);
		}
	}
}

static void deadline_write_hlt_test(uint64_t deadlines[], size_t nr_deadlines)
{
	int i;

	for (i = 0; i < nr_deadlines; i++) {
		uint64_t deadline = deadlines[i];
		uint64_t val;

		reap_interrupt();

		GUEST_ASSERT(deadline);

		atomic_store(&timer_interrupted, 0);
		GUEST_ASSERT(!wrmsr_safe(MSR_IA32_TSC_DEADLINE, deadline));

		GUEST_ASSERT(!rdmsr_safe(MSR_IA32_TSC_DEADLINE, &val));
		GUEST_ASSERT(val == deadline || val == 0);
		GUEST_ASSERT_EQ(atomic_load(&timer_interrupted), 0);

		asm volatile ("sti; hlt; nop; cli"
			      /* L1 exit handler doesn't preserve GP registers. */
			      : : : "cc", "memory",
				"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
				"r8", "r9", "r10", "r11", "r12", "r13", "r14",
				"r15");

		GUEST_ASSERT(!rdmsr_safe(MSR_IA32_TSC_DEADLINE, &val));
		GUEST_ASSERT_EQ(val, 0);
		GUEST_ASSERT_EQ(atomic_load(&timer_interrupted), 1);
	}
}

static void deadline_no_int_test(void)
{
	uint64_t tsc = rdtsc();
	uint64_t deadlines[] = {
		0ull,
		/* big values > tsc. */
		max(~0ull - tsc, ~0ull / 2 + tsc / 2),
		~0ull - 1,
		~0ull - 2,
		~0ull,
	};

	deadline_write_test(false, false, deadlines, ARRAY_SIZE(deadlines));
}

static void __deadline_int_test(bool do_interrupt, bool mask)
{
	uint64_t tsc = rdtsc();
	uint64_t deadlines[] = {
		0ull,
		1ull,
		2ull,
		/* 1 msec past. tsc /2 is to avoid underflow. */
		min(tsc - guest_tsc_khz, tsc / 2 + 1),
		tsc,
	};

	deadline_write_test(do_interrupt, mask, deadlines, ARRAY_SIZE(deadlines));
}

static void deadline_int_test(void)
{
	__deadline_int_test(true, false);
}

static void deadline_int_mask_test(void)
{
	__deadline_int_test(true, true);
}

static void deadline_hlt_test(void)
{
	uint64_t tsc_khz = nested ? l2_tsc_khz : guest_tsc_khz;
	uint64_t tsc = rdtsc();
	/* 1 msec future. */
	uint64_t future = tsc + tsc_khz + 1;
	uint64_t deadlines[] = {
		1ull,
		2ull,
		/* pick a positive value between [0, tsc]. */
		tsc > tsc_khz ? tsc - tsc_khz : tsc / 2 + 1,
		tsc,
		/* If overflow, pick near future value > tsc. */
		future > tsc ? future : ~0ull / 2 + tsc / 2,
	};

	deadline_write_hlt_test(deadlines, ARRAY_SIZE(deadlines));
}

static void guest_code(void)
{
	x2apic_enable();

	x2apic_write_reg(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | TIMER_VECTOR);
	deadline_no_int_test();
	deadline_int_test();
	deadline_hlt_test();

	/* L1 doesn't emulate LVTT entry so that mask is not supported. */
	if (!nested) {
		x2apic_write_reg(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE |
				 APIC_LVT_MASKED | TIMER_VECTOR);
		deadline_no_int_test();
		deadline_int_mask_test();
	}

	if (nested)
		vmcall();
	else
		GUEST_DONE();
}

static void skip_guest_instruction(void)
{
	uint64_t guest_rip, length;

	GUEST_ASSERT(!vmread(GUEST_RIP, &guest_rip));
	GUEST_ASSERT(!vmread(VM_EXIT_INSTRUCTION_LEN, &length));

	GUEST_ASSERT(!vmwrite(GUEST_RIP, guest_rip + length));
	GUEST_ASSERT(!vmwrite(VM_EXIT_INSTRUCTION_LEN, 0));
}

static void l1_guest_code(struct vmx_pages *vmx_pages)
{
	union vmx_ctrl_msr ctls_msr, ctls2_msr;
	uint64_t pin, ctls, ctls2, ctls3;
	bool launch, done;

	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));
	GUEST_ASSERT(load_vmcs(vmx_pages));
	prepare_vmcs(vmx_pages, guest_code,
		     &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	/* Check prerequisites */
	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS, &ctls_msr.val));
	GUEST_ASSERT(ctls_msr.clr & CPU_BASED_HLT_EXITING);
	GUEST_ASSERT(ctls_msr.clr & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS);
	GUEST_ASSERT(ctls_msr.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS);

	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS2, &ctls2_msr.val));
	GUEST_ASSERT(ctls2_msr.clr & SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY);

	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS3, &ctls3));
	GUEST_ASSERT(ctls3 & TERTIARY_EXEC_GUEST_APIC_TIMER);

	pin = vmreadz(PIN_BASED_VM_EXEC_CONTROL);
	pin |= PIN_BASED_EXT_INTR_MASK;
	GUEST_ASSERT(!vmwrite(PIN_BASED_VM_EXEC_CONTROL, pin));

	ctls = vmreadz(CPU_BASED_VM_EXEC_CONTROL);
	ctls |= CPU_BASED_HLT_EXITING | CPU_BASED_USE_TSC_OFFSETTING |
		CPU_BASED_USE_MSR_BITMAPS | CPU_BASED_TPR_SHADOW |
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS |
		CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
	GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));

	/* guest apic timer requires virtual interrutp delivery */
	ctls2 = vmreadz(SECONDARY_VM_EXEC_CONTROL);
	ctls2 |= SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |
		SECONDARY_EXEC_APIC_REGISTER_VIRT |
		SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls2);

	ctls3 = vmreadz(TERTIARY_VM_EXEC_CONTROL);
	ctls3 |= TERTIARY_EXEC_GUEST_APIC_TIMER;
	GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls3));

	/*
	 * We don't emulate apic registers(including APIC_LVTT) for simplicity.
	 * Directly set vector for timer interrupt instead.
	 */
	GUEST_ASSERT(!vmwrite(GUEST_APIC_TIMER_VECTOR, TIMER_VECTOR));

	GUEST_ASSERT(!vmwrite(TSC_OFFSET, l2_tsc_offset));
	if (l2_tsc_multiplier) {
		GUEST_ASSERT(!vmwrite(TSC_MULTIPLIER, l2_tsc_multiplier));

		GUEST_ASSERT(!vmread(SECONDARY_VM_EXEC_CONTROL, &ctls2));
		ctls2 |= SECONDARY_EXEC_TSC_SCALING;
		GUEST_ASSERT(!vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls2));
	} else {
		GUEST_ASSERT(!vmread(SECONDARY_VM_EXEC_CONTROL, &ctls2));
		ctls2 &= ~SECONDARY_EXEC_TSC_SCALING;
		GUEST_ASSERT(!vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls2));
	}

	/* launch L2 */
	launch = true;
	done = false;

	while (!done) {
		uint64_t reason;

		if (launch) {
			GUEST_ASSERT(!vmlaunch());
			launch = false;
		} else
			GUEST_ASSERT(!vmresume());

		GUEST_ASSERT(!vmread(VM_EXIT_REASON, &reason));

		switch (reason) {
		case EXIT_REASON_HLT: {
			uint64_t phy, tsc;

			skip_guest_instruction();
			GUEST_ASSERT(!vmread(GUEST_DEADLINE_PHY, &phy));

			/* Don't wait for more than 1 sec. */
			tsc = rdtsc();
			if (tsc < phy && tsc < ~0ULL - guest_tsc_khz)
				GUEST_ASSERT(tsc + guest_tsc_khz * 1000 >= tsc);

			while (tsc <= phy)
				tsc = rdtsc();
			break;
		}
		case EXIT_REASON_VMCALL:
			done = true;
			break;
		default:
			GUEST_FAIL("unexpected exit reason 0x%lx", reason);
			break;
		}
	}

	GUEST_DONE();
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	bool done = false;

	while (!done) {
		struct ucall uc;

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_SYNC:
			break;
		case UCALL_PRINTF:
			pr_info("%s", uc.buffer);
			break;
		case UCALL_DONE:
			done = true;
			break;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}
	}
}

static int test_tsc_deadline(bool tsc_offset, uint64_t guest_tsc_khz__)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	if (nested) {
		vm_vaddr_t vmx_pages_gva = 0;

		vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);
		vcpu_alloc_vmx(vm, &vmx_pages_gva);
		vcpu_args_set(vcpu, 1, vmx_pages_gva);
	} else
		vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	if (guest_tsc_khz__) {
		uint64_t l1_tsc_multiplier;
		int ret;

		ret = __vcpu_ioctl(vcpu, KVM_SET_TSC_KHZ, (void *)guest_tsc_khz__);
		if (ret) {
			kvm_vm_free(vm);
			return ret;
		}

		guest_tsc_khz = guest_tsc_khz__;

		/*
		 * Pick same to L1 tsc multplier.  Any value to exercise
		 * corner cases is okay.
		 */
		l1_tsc_multiplier = ((__uint128_t)guest_tsc_khz__ *
				     (1ULL << 48)) / host_tsc_khz;
		l2_tsc_multiplier = l1_tsc_multiplier;
		/*
		 * l1_multiplier * l2_multiplier needs to be represented in
		 * the host.
		 */
		if ((__uint128_t)l1_tsc_multiplier * l2_tsc_multiplier >
		    ((__uint128_t)1ULL << (63 + 48))) {

			l2_tsc_multiplier = ((__uint128_t)1ULL << (63 + 48)) /
				l1_tsc_multiplier;
			if (!l2_tsc_multiplier)
				l1_tsc_multiplier = 1;
		}

		l2_tsc_khz = ((__uint128_t)l2_tsc_multiplier * guest_tsc_khz__) >> 48;
		if (!l2_tsc_khz) {
			l2_tsc_multiplier = 1ULL << 48;
			l2_tsc_khz = guest_tsc_khz__;
		}
	} else
		l2_tsc_khz = host_tsc_khz;

	if (tsc_offset) {
		uint64_t offset;

		__TEST_REQUIRE(!__vcpu_has_device_attr(vcpu, KVM_VCPU_TSC_CTRL,
						       KVM_VCPU_TSC_OFFSET),
			       "KVM_VCPU_TSC_OFFSET not supported; skipping test");

		/*
		 * Make the conversion guest deadline virt(L1) => phy (l0)
		 * can overflow/underflow.
		 */
		offset = -rdtsc();
		vcpu_device_attr_set(vcpu, KVM_VCPU_TSC_CTRL,
				     KVM_VCPU_TSC_OFFSET, &offset);

		/* Pick a non-zero value */
		l2_tsc_offset = offset;
	}

	vcpu_set_cpuid_feature(vcpu, X86_FEATURE_TSC_DEADLINE_TIMER);
	vm_install_exception_handler(vm, TIMER_VECTOR,
				     guest_timer_interrupt_handler);

	sync_global_to_guest(vm, host_tsc_khz);
	sync_global_to_guest(vm, guest_tsc_khz);
	sync_global_to_guest(vm, nested);
	sync_global_to_guest(vm, l2_tsc_offset);
	sync_global_to_guest(vm, l2_tsc_multiplier);
	sync_global_to_guest(vm, l2_tsc_khz);
	run_vcpu(vcpu);

	kvm_vm_free(vm);

	l2_tsc_offset = 0;
	l2_tsc_multiplier = 0;
	l2_tsc_khz = 0;

	return 0;
}

static void test(void)
{
	uint64_t guest_tsc_khz__;
	int r;

	test_tsc_deadline(false, 0);
	test_tsc_deadline(true, 0);

	for (guest_tsc_khz__ = host_tsc_khz; guest_tsc_khz__ > 0;
	     guest_tsc_khz__ >>= 1) {
		r = test_tsc_deadline(false, guest_tsc_khz__);
		if (r)
			break;

		test_tsc_deadline(true, guest_tsc_khz__);
	}

	for (guest_tsc_khz__ = host_tsc_khz; guest_tsc_khz__ < max_guest_tsc_khz;
	     guest_tsc_khz__ <<= 1) {
		r = test_tsc_deadline(false, guest_tsc_khz__);
		if (r)
			break;

		test_tsc_deadline(true, guest_tsc_khz__);
	}

	test_tsc_deadline(false, max_guest_tsc_khz);
	test_tsc_deadline(true, max_guest_tsc_khz);
}

int main(int argc, char *argv[])
{
	uint32_t eax_denominator, ebx_numerator, ecx_hz, edx;
	union vmx_ctrl_msr ctls;
	uint64_t ctls3;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_X2APIC));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SERIALIZE));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_TSC_CONTROL));

	cpuid(0x15, &eax_denominator, &ebx_numerator, &ecx_hz, &edx);
	TEST_REQUIRE(ebx_numerator > 0);
	TEST_REQUIRE(eax_denominator > 0);

	if (ecx_hz > 0)
		host_tsc_khz = ecx_hz * ebx_numerator / eax_denominator / 1000;
	else {
		uint32_t eax_base_mhz, ebx, ecx;

		cpuid(0x16, &eax_base_mhz, &ebx, &ecx, &edx);
		host_tsc_khz = eax_base_mhz * 1000 * ebx_numerator /
			eax_denominator;
	}
	TEST_REQUIRE(host_tsc_khz > 0);

	/* See arch/x86/kvm/{x86.c, vmx/vmx.c}. There is no way for userspace to retrieve it. */
#define KVM_VMX_TSC_MULTIPLIER_MAX	0xffffffffffffffffULL
	max_guest_tsc_khz = min((uint64_t)0x7fffffffULL,
				mul_u64_u32_shr(KVM_VMX_TSC_MULTIPLIER_MAX, host_tsc_khz, 48));

	test();

	ctls.val = kvm_get_feature_msr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
	if (ctls.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS)
		ctls3 = kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS3);
	else
		ctls3 = 0;
	if (kvm_cpu_has(X86_FEATURE_VMX) &&
	    ctls3 & TERTIARY_EXEC_GUEST_APIC_TIMER) {
		nested = true;
		test();
	}

	return 0;
}
