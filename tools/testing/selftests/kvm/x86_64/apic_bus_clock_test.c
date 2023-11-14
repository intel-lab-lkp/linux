// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE /* for program_invocation_short_name */

#include "apic.h"
#include "test_util.h"

/* Pick one convenient value, 1Ghz.  No special meaning. */
#define TSC_HZ			(1 * 1000 * 1000 * 1000ULL)

/* Wait for 100 msec, not too long, not too short value. */
#define LOOP_MSEC		100ULL
#define TSC_WAIT_DELTA		(TSC_HZ / 1000 * LOOP_MSEC)

/* Pick up typical value.  Different enough from the default value, 1GHz.  */
#define APIC_BUS_CLOCK_FREQ	(25 * 1000 * 1000ULL)

static void guest_code(void)
{
	/* Possible tdcr values and its divide count. */
	struct {
		u32 tdcr;
		u32 divide_count;
	} tdcrs[] = {
		{0x0, 2},
		{0x1, 4},
		{0x2, 8},
		{0x3, 16},
		{0x8, 32},
		{0x9, 64},
		{0xa, 128},
		{0xb, 1},
	};

	u32 tmict, tmcct;
	u64 tsc0, tsc1;
	int i;

	asm volatile("cli");

	xapic_enable();

	/*
	 * Setup one-shot timer.  Because we don't fire the interrupt, the
	 * vector doesn't matter.
	 */
	xapic_write_reg(APIC_LVT0, APIC_LVT_TIMER_ONESHOT);

	for (i = 0; i < ARRAY_SIZE(tdcrs); i++) {
		xapic_write_reg(APIC_TDCR, tdcrs[i].tdcr);

		/* Set the largest value to not trigger the interrupt. */
		tmict = ~0;
		xapic_write_reg(APIC_TMICT, tmict);

		/* Busy wait for LOOP_MSEC */
		tsc0 = rdtsc();
		tsc1 = tsc0;
		while (tsc1 - tsc0 < TSC_WAIT_DELTA)
			tsc1 = rdtsc();

		/* Read apic timer and tsc */
		tmcct = xapic_read_reg(APIC_TMCCT);
		tsc1 = rdtsc();

		/* Stop timer */
		xapic_write_reg(APIC_TMICT, 0);

		/* Report it. */
		GUEST_SYNC_ARGS(tdcrs[i].divide_count, tmict - tmcct,
				tsc1 - tsc0, 0, 0);
	}

	GUEST_DONE();
}

void test_apic_bus_clock(struct kvm_vcpu *vcpu)
{
	bool done = false;
	struct ucall uc;

	while (!done) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			done = true;
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_SYNC: {
			u32 divide_counter = uc.args[1];
			u32 apic_cycles = uc.args[2];
			u64 tsc_cycles = uc.args[3];
			u64 freq;

			TEST_ASSERT(tsc_cycles > 0,
				    "tsc cycles must not be zero.");

			/* Allow 1% slack. */
			freq = apic_cycles * divide_counter * TSC_HZ / tsc_cycles;
			TEST_ASSERT(freq < APIC_BUS_CLOCK_FREQ * 101 / 100,
				    "APIC bus clock frequency is too large");
			TEST_ASSERT(freq > APIC_BUS_CLOCK_FREQ * 99 / 100,
				    "APIC bus clock frequency is too small");
			break;
		}
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;

	vm = __vm_create(VM_MODE_DEFAULT, 1, 0);
	vm_ioctl(vm, KVM_SET_TSC_KHZ, (void *) (TSC_HZ / 1000));
	/*  KVM_CAP_X86_BUS_FREQUENCY_CONTROL requires that no vcpu is created. */
	vm_enable_cap(vm, KVM_CAP_X86_BUS_FREQUENCY_CONTROL,
		      APIC_BUS_CLOCK_FREQ);
	vcpu = vm_vcpu_add(vm, 0, guest_code);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	test_apic_bus_clock(vcpu);
	kvm_vm_free(vm);
}
