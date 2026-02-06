// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Loongson Technology Corporation Limited
 */

#include <linux/kvm_host.h>
#include <trace/events/kvm.h>
#include <asm/kvm_pch_pic.h>
#include <asm/kvm_vcpu.h>

static int kvm_set_pic_irq(struct kvm_kernel_irq_routing_entry *e,
		struct kvm *kvm, int irq_source_id, int level, bool line_status)
{
	/* PCH-PIC pin (0 ~ 64) <---> GSI (0 ~ 64) */
	pch_pic_set_irq(kvm->arch.pch_pic, e->irqchip.pin, level);

	return 0;
}

static int kvm_dmsintc_set_msi_irq(struct kvm *kvm, u32 addr, int data, int level)
{
	unsigned int virq, dest;
	struct kvm_vcpu *vcpu;

	virq = (addr >> AVEC_IRQ_SHIFT) & AVEC_IRQ_MASK;
	dest = (addr >> AVEC_CPU_SHIFT) & kvm->arch.dmsintc->cpu_mask;
	if (dest > KVM_MAX_VCPUS)
		return -EINVAL;
	vcpu = kvm_get_vcpu_by_cpuid(kvm, dest);
	if (!vcpu)
		return -EINVAL;
	return kvm_loongarch_deliver_msi_to_vcpu(kvm, vcpu, virq, level);
}

static int loongarch_set_msi(struct kvm_kernel_irq_routing_entry *e,
			struct kvm *kvm, int level)
{
	u64 msg_addr;

	msg_addr = (((u64)e->msi.address_hi) << 32) | e->msi.address_lo;
	if (cpu_has_msgint && kvm->arch.dmsintc &&
		msg_addr >= kvm->arch.dmsintc->msg_addr_base &&
		msg_addr < (kvm->arch.dmsintc->msg_addr_base  + kvm->arch.dmsintc->msg_addr_size)) {
		return kvm_dmsintc_set_msi_irq(kvm, msg_addr, e->msi.data, level);
	} else {
		pch_msi_set_irq(kvm, e->msi.data, level);
	}

	return 0;
}

/*
 * kvm_set_msi: inject the MSI corresponding to the
 * MSI routing entry
 *
 * This is the entry point for irqfd MSI injection
 * and userspace MSI injection.
 */
int kvm_set_msi(struct kvm_kernel_irq_routing_entry *e,
		struct kvm *kvm, int irq_source_id, int level, bool line_status)
{
	if (!level)
		return -1;

	return loongarch_set_msi(e, kvm, level);
}

/*
 * kvm_set_routing_entry: populate a kvm routing entry
 * from a user routing entry
 *
 * @kvm: the VM this entry is applied to
 * @e: kvm kernel routing entry handle
 * @ue: user api routing entry handle
 * return 0 on success, -EINVAL on errors.
 */
int kvm_set_routing_entry(struct kvm *kvm,
			struct kvm_kernel_irq_routing_entry *e,
			const struct kvm_irq_routing_entry *ue)
{
	switch (ue->type) {
	case KVM_IRQ_ROUTING_IRQCHIP:
		e->set = kvm_set_pic_irq;
		e->irqchip.irqchip = ue->u.irqchip.irqchip;
		e->irqchip.pin = ue->u.irqchip.pin;

		if (e->irqchip.pin >= KVM_IRQCHIP_NUM_PINS)
			return -EINVAL;

		return 0;
	case KVM_IRQ_ROUTING_MSI:
		e->set = kvm_set_msi;
		e->msi.address_lo = ue->u.msi.address_lo;
		e->msi.address_hi = ue->u.msi.address_hi;
		e->msi.data = ue->u.msi.data;
		return 0;
	default:
		return -EINVAL;
	}
}

int kvm_arch_set_irq_inatomic(struct kvm_kernel_irq_routing_entry *e,
		struct kvm *kvm, int irq_source_id, int level, bool line_status)
{
	if (!level)
		return -EWOULDBLOCK;

	switch (e->type) {
	case KVM_IRQ_ROUTING_IRQCHIP:
		pch_pic_set_irq(kvm->arch.pch_pic, e->irqchip.pin, level);
		return 0;
	case KVM_IRQ_ROUTING_MSI:
		loongarch_set_msi(e, kvm, level);
		return 0;
	default:
		return -EWOULDBLOCK;
	}
}

bool kvm_arch_intc_initialized(struct kvm *kvm)
{
	return kvm_arch_irqchip_in_kernel(kvm);
}
