// SPDX-License-Identifier: GPL-2.0-only
/*
 * Regression tests for in-kernel I/O APIC state.
 */

#include "apic.h"
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define TEST_IOAPIC_PIN		16
#define TEST_VECTOR		0x50
#define NO_SUCH_APIC_ID		0xfe
#define TEST_IOAPIC_EDGE_TRIG	0
#define TEST_IOAPIC_LEVEL_TRIG	1

static void get_ioapic(struct kvm_vm *vm, struct kvm_irqchip *irqchip)
{
	int r;

	irqchip->chip_id = KVM_IRQCHIP_IOAPIC;
	r = __vm_ioctl(vm, KVM_GET_IRQCHIP, irqchip);
	if (r && errno == ENXIO)
		__TEST_REQUIRE(0, "In-kernel I/O APIC not available");

	TEST_ASSERT(!r, KVM_IOCTL_ERROR(KVM_GET_IRQCHIP, r));
}

static void set_ioapic(struct kvm_vm *vm, struct kvm_irqchip *irqchip)
{
	irqchip->chip_id = KVM_IRQCHIP_IOAPIC;
	vm_ioctl(vm, KVM_SET_IRQCHIP, irqchip);
}

static void set_ioapic_entry(struct kvm_vm *vm, bool level_triggered,
			     u32 dest_id)
{
	struct kvm_irqchip irqchip;

	get_ioapic(vm, &irqchip);

	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.vector =
		TEST_VECTOR;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.dest_id =
		dest_id;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.dest_mode = 0;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.trig_mode =
		level_triggered ? TEST_IOAPIC_LEVEL_TRIG :
				  TEST_IOAPIC_EDGE_TRIG;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.mask = 0;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.remote_irr = 0;

	set_ioapic(vm, &irqchip);
}

static int kvm_irq_line_status(struct kvm_vm *vm, int level)
{
	struct kvm_irq_level irq = {
		.irq = TEST_IOAPIC_PIN,
		.level = level,
	};

	vm_ioctl(vm, KVM_IRQ_LINE_STATUS, &irq);
	return irq.status;
}

static void assert_ioapic_pin_irr(struct kvm_vm *vm, bool expected)
{
	struct kvm_irqchip irqchip;

	get_ioapic(vm, &irqchip);
	TEST_ASSERT(!!(irqchip.chip.ioapic.irr & (1 << TEST_IOAPIC_PIN)) == expected,
		    "Expected IOAPIC IRR for pin %u to be %u, got 0x%x",
		    TEST_IOAPIC_PIN, expected, irqchip.chip.ioapic.irr);
}

static void test_no_remote_irr_for_undelivered_level_interrupt(void)
{
	struct kvm_irqchip irqchip;
	struct kvm_vm *vm;
	int status;

	vm = vm_create_barebones();
	vm_create_irqchip(vm);

	set_ioapic_entry(vm, true, NO_SUCH_APIC_ID);

	status = kvm_irq_line_status(vm, 1);
	TEST_ASSERT(status == -1,
		    "Expected failed interrupt delivery, got %d", status);

	get_ioapic(vm, &irqchip);
	TEST_ASSERT(!irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.remote_irr,
		    "KVM set remote_irr for a level-triggered interrupt that wasn't delivered");

	kvm_vm_free(vm);
}

static void test_duplicate_edge_interrupt_preserves_delivery_state(void)
{
	struct kvm_lapic_state lapic;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	u64 apicbase;
	int status;

	vm = vm_create_with_one_vcpu(&vcpu, NULL);
	apicbase = vcpu_get_msr(vcpu, MSR_IA32_APICBASE);
	vcpu_set_msr(vcpu, MSR_IA32_APICBASE,
		     apicbase | MSR_IA32_APICBASE_ENABLE);
	vcpu_ioctl(vcpu, KVM_GET_LAPIC, &lapic);
	*(u32 *)(lapic.regs + APIC_SPIV) |= APIC_SPIV_APIC_ENABLED;
	vcpu_ioctl(vcpu, KVM_SET_LAPIC, &lapic);

	set_ioapic_entry(vm, false, vcpu->id);

	status = kvm_irq_line_status(vm, 1);
	TEST_ASSERT(status > 0,
		    "Expected edge interrupt delivery, got %d", status);

	assert_ioapic_pin_irr(vm, false);

	status = kvm_irq_line_status(vm, 1);
	TEST_ASSERT(!status,
		    "Expected duplicate edge interrupt to be coalesced, got %d",
		    status);

	assert_ioapic_pin_irr(vm, false);

	kvm_vm_free(vm);
}

int main(void)
{
	test_no_remote_irr_for_undelivered_level_interrupt();
	test_duplicate_edge_interrupt_preserves_delivery_state();

	return 0;
}
