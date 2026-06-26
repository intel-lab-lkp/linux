// SPDX-License-Identifier: GPL-2.0-only
/*
 * Weak default function implementations for KVM eventfd.
 *
 * The main eventfd logic is implemented in Rust (rust/kernel/kvm/eventfd.rs).
 * This file provides only the __weak default implementations that arch code
 * may override with strong definitions.
 */

#include <linux/kvm_host.h>
#include <linux/kvm_irqfd.h>
#include <linux/irqbypass.h>

bool __weak kvm_arch_irqfd_allowed(struct kvm *kvm, struct kvm_irqfd *args)
{
	return true;
}

int __weak kvm_arch_set_irq_inatomic(
				struct kvm_kernel_irq_routing_entry *irq,
				struct kvm *kvm, int irq_source_id,
				int level,
				bool line_status)
{
	return -EWOULDBLOCK;
}

#if IS_ENABLED(CONFIG_HAVE_KVM_IRQ_BYPASS)
void __weak kvm_arch_irq_bypass_stop(
				struct irq_bypass_consumer *cons)
{
}

void __weak kvm_arch_irq_bypass_start(
				struct irq_bypass_consumer *cons)
{
}

void __weak kvm_arch_update_irqfd_routing(struct kvm_kernel_irqfd *irqfd,
					  struct kvm_kernel_irq_routing_entry *old,
					  struct kvm_kernel_irq_routing_entry *new)
{
}
#endif

/*
 * The Rust implementation provides kvm_irq_has_notifier via #[no_mangle].
 * Symbol export for KVM-internal use must be done from C.
 */
extern bool kvm_irq_has_notifier(struct kvm *kvm, unsigned int irqchip,
				 unsigned int pin);
EXPORT_SYMBOL_FOR_KVM_INTERNAL(kvm_irq_has_notifier);
