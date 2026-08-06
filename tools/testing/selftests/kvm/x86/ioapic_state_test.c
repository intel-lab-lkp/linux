// SPDX-License-Identifier: GPL-2.0-only
/*
 * Regression tests for in-kernel I/O APIC state.
 */

#include "kvm_util.h"
#include "test_util.h"

#define TEST_IOAPIC_PIN		16
#define TEST_VECTOR		0x50
#define NO_SUCH_APIC_ID		0xfe

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

static void test_no_remote_irr_for_undelivered_interrupt(void)
{
	struct kvm_irq_level irq = {
		.irq = TEST_IOAPIC_PIN,
		.level = 1,
	};
	struct kvm_irqchip irqchip;
	struct kvm_vm *vm;

	vm = vm_create_barebones();
	vm_create_irqchip(vm);

	get_ioapic(vm, &irqchip);

	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.vector = TEST_VECTOR;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.dest_id = NO_SUCH_APIC_ID;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.dest_mode = 0;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.trig_mode = 1;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.mask = 0;
	irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.remote_irr = 0;

	set_ioapic(vm, &irqchip);

	vm_ioctl(vm, KVM_IRQ_LINE_STATUS, &irq);
	TEST_ASSERT(irq.status == -1,
		    "Expected failed interrupt delivery, got %d", irq.status);

	get_ioapic(vm, &irqchip);
	TEST_ASSERT(!irqchip.chip.ioapic.redirtbl[TEST_IOAPIC_PIN].fields.remote_irr,
		    "KVM set remote_irr for a level-triggered interrupt that wasn't delivered");

	kvm_vm_free(vm);
}

int main(void)
{
	test_no_remote_irr_for_undelivered_interrupt();

	return 0;
}
