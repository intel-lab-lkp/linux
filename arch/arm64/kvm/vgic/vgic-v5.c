// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Arm Ltd.
 */

#include <kvm/arm_vgic.h>
#include <linux/irqchip/arm-vgic-info.h>

#include "vgic.h"

/*
 * Probe for a vGICv5 compatible interrupt controller, returning 0 on success.
 * Currently only supports GICv3-based VMs on a GICv5 host, and hence only
 * registers a VGIC_V3 device.
 */
int vgic_v5_probe(const struct gic_kvm_info *info)
{
	u64 ich_vtr_el2;
	int ret;

	if (!cpus_have_final_cap(ARM64_HAS_GICV5_LEGACY))
		return -ENODEV;

	kvm_vgic_global_state.type = VGIC_V5;
	kvm_vgic_global_state.has_gcie_v3_compat = true;

	/* We only support v3 compat mode - use vGICv3 limits */
	kvm_vgic_global_state.max_gic_vcpus = VGIC_V3_MAX_CPUS;

	kvm_vgic_global_state.vcpu_base = 0;
	kvm_vgic_global_state.vctrl_base = NULL;
	kvm_vgic_global_state.can_emulate_gicv2 = false;
	kvm_vgic_global_state.has_gicv4 = false;
	kvm_vgic_global_state.has_gicv4_1 = false;

	ich_vtr_el2 =  kvm_call_hyp_ret(__vgic_v3_get_gic_config);
	kvm_vgic_global_state.ich_vtr_el2 = (u32)ich_vtr_el2;

	/*
	 * The ListRegs field is 5 bits, but there is an architectural
	 * maximum of 16 list registers. Just ignore bit 4...
	 */
	kvm_vgic_global_state.nr_lr = (ich_vtr_el2 & 0xf) + 1;

	ret = kvm_register_vgic_device(KVM_DEV_TYPE_ARM_VGIC_V3);
	if (ret) {
		kvm_err("Cannot register GICv3-legacy KVM device.\n");
		return ret;
	}

	static_branch_enable(&kvm_vgic_global_state.gicv3_cpuif);
	kvm_info("GCIE legacy system register CPU interface\n");

	return 0;
}

static void vgic_v5_construct_hmrs(struct kvm_vcpu *vcpu);

void vgic_v5_reset(struct kvm_vcpu *vcpu)
{
	u64 idr0;

	idr0 = read_sysreg_s(SYS_ICC_IDR0_EL1);
	switch (FIELD_GET(ICC_IDR0_EL1_ID_BITS, idr0)) {
	case ICC_IDR0_EL1_ID_BITS_16BITS:
		vcpu->arch.vgic_cpu.num_id_bits = 16;
		break;
	case ICC_IDR0_EL1_ID_BITS_24BITS:
		vcpu->arch.vgic_cpu.num_id_bits = 24;
		break;
	default:
		pr_warn("unknown value for id_bits");
		vcpu->arch.vgic_cpu.num_id_bits = 16;
	}

	switch (FIELD_GET(ICC_IDR0_EL1_PRI_BITS, idr0)) {
	case ICC_IDR0_EL1_PRI_BITS_4BITS:
		vcpu->arch.vgic_cpu.num_pri_bits = 4;
		break;
	case ICC_IDR0_EL1_PRI_BITS_5BITS:
		vcpu->arch.vgic_cpu.num_pri_bits = 5;
		break;
	default:
		pr_warn("unknown value for priority_bits");
		vcpu->arch.vgic_cpu.num_pri_bits = 4;
	}

	/*
	 * We're now ready to run this VCPU so no more changes to the
	 * PPI config are expected.
	 */
	vgic_v5_construct_hmrs(vcpu);

}

int vgic_v5_init(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	unsigned long idx;

	if (vgic_initialized(kvm))
		return 0;

	kvm_for_each_vcpu(idx, vcpu, kvm) {
		if (vcpu_has_nv(vcpu)) {
			kvm_err("Nested GICv5 VMs are currently unsupported\n");
			return -EINVAL;
		}
	}

	return 0;
}

int vgic_v5_map_resources(struct kvm *kvm)
{
	if (!vgic_initialized(kvm))
		return -EBUSY;

	return 0;
}

static u32 vgic_v5_get_effective_priority_mask(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	unsigned highest_ap, priority_mask;

	/*
	 * Counting the number of trailing zeros gives the current
	 * active priority. Explicitly use the 32-bit version here as
	 * we have 32 priorities. 0x20 then means that there are no
	 * active priorities.
	 */
	highest_ap = __builtin_ctz(cpu_if->vgic_apr);

	/*
	 * An interrupt is of sufficient priority if it is equal to or
	 * greater than the priority mask. Add 1 to the priority mask
	 * (i.e., lower priority) to match the APR logic before taking
	 * the min. This gives us the lowest priority that is masked.
	 */
	priority_mask = FIELD_GET(FEAT_GCIE_ICH_VMCR_EL2_VPMR, cpu_if->vgic_vmcr);
	priority_mask = min(highest_ap, priority_mask + 1);

	return priority_mask;
}

static void vgic_v5_construct_hmrs(struct kvm_vcpu *vcpu)
{
	/*
	 * Calculate the PPI HMR to present to the guest (and for
	 * internal interrupt masking).
	 */
	vcpu->arch.vgic_cpu.vgic_v5.vgic_ppi_hmr[0] = 0;
	vcpu->arch.vgic_cpu.vgic_v5.vgic_ppi_hmr[1] = 0;
	for (int i = 0; i < VGIC_V5_NR_PRIVATE_IRQS; ++i) {
		int reg = i / 64;
		u64 bit = BIT_ULL(i % 64);
		struct vgic_irq *irq = &vcpu->arch.vgic_cpu.private_irqs[i];

		raw_spin_lock(&irq->irq_lock);

		if (irq->config == VGIC_CONFIG_LEVEL)
			vcpu->arch.vgic_cpu.vgic_v5.vgic_ppi_hmr[reg] |= bit;

		raw_spin_unlock(&irq->irq_lock);
	}
}

bool vgic_v5_ppi_set_pending_state(struct kvm_vcpu *vcpu,
				   struct vgic_irq *irq)
{
	struct vgic_v5_cpu_if *cpu_if;
	const u32 id_bit = BIT_ULL(irq->intid % 64);
	const u32 reg = FIELD_GET(GICV5_HWIRQ_ID, irq->intid) / 64;

	if (!vcpu || !irq)
		return false;

	/* Skip injecting the state altogether */
	if (irq->directly_injected)
		return true;

	cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	if (irq_is_pending(irq))
		cpu_if->vgic_ppi_pendr[reg] |= id_bit;
	else
		cpu_if->vgic_ppi_pendr[reg] &= ~id_bit;

	return true;
}

/*
 * For GICv5, the PPIs are mostly directly managed by the hardware. We
 * (the hypervisor) handle the pending, active, enable state
 * save/restore, but don't need the PPIs to be queued on a per-VCPU AP
 * list. Therefore, sanity check the state, unlock, and return.
 */
bool vgic_v5_ppi_queue_irq_unlock(struct kvm *kvm, struct vgic_irq *irq,
				  unsigned long flags)
	__releases(&irq->irq_lock)
{
	struct kvm_vcpu *vcpu;

	lockdep_assert_held(&irq->irq_lock);

	if (WARN_ON_ONCE(!irq_is_ppi_v5(irq->intid)))
		return false;

	vcpu = irq->target_vcpu;
	if (WARN_ON_ONCE(!vcpu))
		return false;

	raw_spin_unlock_irqrestore(&irq->irq_lock, flags);

	/* Directly kick the target VCPU to make sure it sees the IRQ */
	kvm_make_request(KVM_REQ_IRQ_PENDING, vcpu);
	kvm_vcpu_kick(vcpu);

	return true;
}

static struct irq_ops vgic_v5_ppi_irq_ops = {
	.set_pending_state = vgic_v5_ppi_set_pending_state,
	.queue_irq_unlock = vgic_v5_ppi_queue_irq_unlock,
};

void vgic_v5_set_ppi_ops(struct vgic_irq *irq)
{
	if (WARN_ON(!irq) || WARN_ON(irq->ops))
		return;

	irq->ops = &vgic_v5_ppi_irq_ops;
}


/*
 * Sync back the PPI priorities to the vgic_irq shadow state
 */
static void vgic_v5_sync_ppi_priorities(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	unsigned long flags;
	int i, reg;

	/* We have 16 PPI Priority regs */
	for (reg = 0; reg < 16; reg++) {
		const unsigned long priorityr = cpu_if->vgic_ppi_priorityr[reg];

		for (i = 0; i < 8; ++i) {
			struct vgic_irq *irq;
			u32 intid;
			u8 priority;

			priority = (priorityr >> (i * 8)) & 0x1f;

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, reg * 8 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);
			raw_spin_lock_irqsave(&irq->irq_lock, flags);

			irq->priority = priority;

			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
			vgic_put_irq(vcpu->kvm, irq);
		}
	}
}

bool vgic_v5_has_pending_ppi(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	unsigned long flags;
	int i, reg;
	unsigned int priority_mask;

	/* If no pending bits are set, exit early */
	if (likely(!cpu_if->vgic_ppi_pendr[0] && !cpu_if->vgic_ppi_pendr[1]))
		return false;

	priority_mask = vgic_v5_get_effective_priority_mask(vcpu);

	/* If the combined priority mask is 0, nothing can be signalled! */
	if (!priority_mask)
		return false;

	/* The shadow priority is only updated on demand, sync it across first */
	vgic_v5_sync_ppi_priorities(vcpu);

	for (reg = 0; reg < 2; reg++) {
		unsigned long possible_bits;
		const unsigned long enabler = cpu_if->vgic_ich_ppi_enabler_exit[reg];
		const unsigned long pendr = cpu_if->vgic_ppi_pendr_exit[reg];
		bool has_pending = false;

		/* Check all interrupts that are enabled and pending */
		possible_bits = enabler & pendr;

		/*
		 * Optimisation: pending and enabled with no active priorities
		 */
		if (possible_bits && priority_mask > 0x1f)
			return true;

		for_each_set_bit(i, &possible_bits, 64) {
			struct vgic_irq *irq;
			u32 intid;

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, reg * 64 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);
			raw_spin_lock_irqsave(&irq->irq_lock, flags);

			/*
			 * We know that the interrupt is enabled and pending, so
			 * only check the priority.
			 */
			if (irq->priority <= priority_mask)
				has_pending = true;

			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
			vgic_put_irq(vcpu->kvm, irq);

			if (has_pending)
				return true;
		}
	}

	return false;
}

/*
 * Detect any PPIs state changes, and propagate the state with KVM's
 * shadow structures.
 */
static void vgic_v5_merge_ppi_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	unsigned long flags;
	int i, reg;

	for (reg = 0; reg < 2; reg++) {
		unsigned long changed_bits;
		const unsigned long enabler = cpu_if->vgic_ich_ppi_enabler_exit[reg];
		const unsigned long activer = cpu_if->vgic_ppi_activer_exit[reg];
		const unsigned long pendr = cpu_if->vgic_ppi_pendr_exit[reg];

		/*
		 * Track what changed across enabler, activer, pendr, but mask
		 * with ~DVI.
		 */
		changed_bits = cpu_if->vgic_ich_ppi_enabler_entry[reg] ^ enabler;
		changed_bits |= cpu_if->vgic_ppi_activer_entry[reg] ^ activer;
		changed_bits |= cpu_if->vgic_ppi_pendr_entry[reg] ^ pendr;
		changed_bits &= ~cpu_if->vgic_ppi_dvir[reg];

		for_each_set_bit(i, &changed_bits, 64) {
			struct vgic_irq *irq;
			u32 intid;

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, reg * 64 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);

			raw_spin_lock_irqsave(&irq->irq_lock, flags);
			irq->enabled = !!(enabler & BIT(i));
			irq->active = !!(activer & BIT(i));
			/* This is an OR to avoid losing incoming edges! */
			if (irq->config == VGIC_CONFIG_EDGE)
				irq->pending_latch |= !!(pendr & BIT(i));
			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);

			vgic_put_irq(vcpu->kvm, irq);
		}

		/* Re-inject the exit state as entry state next time! */
		cpu_if->vgic_ich_ppi_enabler_entry[reg] = enabler;
		cpu_if->vgic_ppi_activer_entry[reg] = activer;

		/*
		 * Pending state is a bit different. We only propagate back
		 * pending state for Edge interrupts. Moreover, this is OR'd
		 * with the incoming state to make sure we don't lose incoming
		 * edges. Use the (inverse) HMR to mask off all Level bits, and
		 * OR.
		 */
		cpu_if->vgic_ppi_pendr[reg] |= pendr & ~cpu_if->vgic_ppi_hmr[reg];
	}
}

void vgic_v5_flush_ppi_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	/*
	 * We're about to enter the guest. Copy the shadow state to the pending
	 * reg that will be written to the ICH_PPI_PENDRx_EL2 regs. While the
	 * guest is running we track any incoming changes to the pending state in
	 * vgic_ppi_pendr. The incoming changes are merged with the outgoing
	 * changes on the return path.
	 */
	cpu_if->vgic_ppi_pendr_entry[0] = cpu_if->vgic_ppi_pendr[0];
	cpu_if->vgic_ppi_pendr_entry[1] = cpu_if->vgic_ppi_pendr[1];

	/*
	 * Make sure that we can correctly detect "edges" in the PPI
	 * state. There's a path where we never actually enter the guest, and
	 * failure to do this risks losing pending state
	 */
	cpu_if->vgic_ppi_pendr_exit[0] = cpu_if->vgic_ppi_pendr[0];
	cpu_if->vgic_ppi_pendr_exit[1] = cpu_if->vgic_ppi_pendr[1];

}

void vgic_v5_fold_irq_state(struct kvm_vcpu *vcpu)
{
	/* Sync back the guest PPI state to the KVM shadow state */
	vgic_v5_merge_ppi_state(vcpu);
}

/*
 * Sets/clears the corresponding bit in the ICH_PPI_DVIR register.
 */
int vgic_v5_set_ppi_dvi(struct kvm_vcpu *vcpu, u32 irq, bool dvi)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u32 ppi = FIELD_GET(GICV5_HWIRQ_ID, irq);

	if (ppi >= 128)
		return -EINVAL;

	if (dvi) {
		/* Set the bit */
		cpu_if->vgic_ppi_dvir[ppi / 64] |= 1UL << (ppi % 64);
	} else {
		/* Clear the bit */
		cpu_if->vgic_ppi_dvir[ppi / 64] &= ~(1UL << (ppi % 64));
	}

	return 0;
}

void vgic_v5_load(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	/*
	 * On the WFI path, vgic_load is called a second time. The first is when
	 * scheduling in the vcpu thread again, and the second is when leaving
	 * WFI. Skip the second instance as it serves no purpose and just
	 * restores the same state again.
	 */
	if (READ_ONCE(cpu_if->gicv5_vpe.resident))
		return;

	kvm_call_hyp(__vgic_v5_restore_vmcr_aprs, cpu_if);

	WRITE_ONCE(cpu_if->gicv5_vpe.resident, true);
}

void vgic_v5_put(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	/*
	 * Do nothing if we're not resident. This can happen in the WFI path
	 * where we do a vgic_put in the WFI path and again later when
	 * descheduling the thread. We risk losing VMCR state if we sync it
	 * twice, so instead return early in this case.
	 */
	if (!READ_ONCE(cpu_if->gicv5_vpe.resident))
		return;

	kvm_call_hyp(__vgic_v5_save_vmcr_aprs, cpu_if);

	WRITE_ONCE(cpu_if->gicv5_vpe.resident, false);
}

void vgic_v5_get_vmcr(struct kvm_vcpu *vcpu, struct vgic_vmcr *vmcrp)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u64 vmcr = cpu_if->vgic_vmcr;

	vmcrp->en = FIELD_GET(FEAT_GCIE_ICH_VMCR_EL2_EN, vmcr);
	vmcrp->pmr = FIELD_GET(FEAT_GCIE_ICH_VMCR_EL2_VPMR, vmcr);
}

void vgic_v5_set_vmcr(struct kvm_vcpu *vcpu, struct vgic_vmcr *vmcrp)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u64 vmcr;

	vmcr = FIELD_PREP(FEAT_GCIE_ICH_VMCR_EL2_VPMR, vmcrp->pmr) |
	       FIELD_PREP(FEAT_GCIE_ICH_VMCR_EL2_EN, vmcrp->en);

	cpu_if->vgic_vmcr = vmcr;
}

void vgic_v5_restore_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	__vgic_v5_restore_icsr(cpu_if);
	kvm_call_hyp(__vgic_v5_restore_ppi_state, cpu_if);
	dsb(sy);
}

void vgic_v5_save_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	__vgic_v5_save_icsr(cpu_if);
	kvm_call_hyp(__vgic_v5_save_ppi_state, cpu_if);
	dsb(sy);
}
