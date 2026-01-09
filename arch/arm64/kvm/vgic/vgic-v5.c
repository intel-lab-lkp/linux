// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, 2026 Arm Ltd.
 */

#include <kvm/arm_vgic.h>
#include <linux/irqchip/arm-vgic-info.h>

#include "vgic.h"

static struct vgic_v5_ppi_caps *ppi_caps;

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

int vgic_v5_finalize_ppi_state(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;

	if (!vgic_is_v5(kvm))
		return 0;

	if (!ppi_caps)
		return -ENXIO;

	/* The PPI state for all VCPUs should be the same. Pick the first. */
	vcpu = kvm_get_vcpu(kvm, 0);

	vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_mask[0] = 0;
	vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_mask[1] = 0;
	vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_hmr[0] = 0;
	vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_hmr[1] = 0;

	for (int i = 0; i < VGIC_V5_NR_PRIVATE_IRQS; i++) {
		int reg = i / 64;
		u64 bit = BIT_ULL(i % 64);
		struct vgic_irq *irq = &vcpu->arch.vgic_cpu.private_irqs[i];

		guard(raw_spinlock_irqsave)(&irq->irq_lock);

		/*
		 * We only expose PPIs with an owner or the SW_PPI to the
		 * guest.
		 */
		if (!irq->owner &&
		    FIELD_GET(GICV5_HWIRQ_ID, irq->intid) != GICV5_ARCH_PPI_SW_PPI)
			continue;

		/*
		 * If the PPI isn't implemented, we can't pass it through to a
		 * guest anyhow.
		 */
		if (!(ppi_caps->impl_ppi_mask[reg] & bit))
			continue;

		vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_mask[reg] |= bit;

		if (irq->config == VGIC_CONFIG_LEVEL)
			vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_hmr[reg] |= bit;
	}

	return 0;
}

/*
 * Not all PPIs are guaranteed to be implemented for GICv5. Deterermine which
 * ones are, and generate a mask.
 */
void vgic_v5_get_implemented_ppis(void)
{
	if (!cpus_have_final_cap(ARM64_HAS_GICV5_CPUIF))
		return;

	/* Never freed again */
	ppi_caps = kzalloc(sizeof(*ppi_caps), GFP_KERNEL);
	if (!ppi_caps)
		return;

	ppi_caps->impl_ppi_mask[0] = 0;
	ppi_caps->impl_ppi_mask[1] = 0;

	/*
	 * If we have KVM, we have EL2, which means that we have support for the
	 * EL1 and EL2 P & V timers.
	 */
	ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_CNTHP);
	ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_CNTV);
	ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_CNTHV);
	ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_CNTP);

	/* The SW_PPI should be available */
	ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_SW_PPI);

	/* The PMUIRQ is available if we have the PMU */
	if (system_supports_pmuv3())
		ppi_caps->impl_ppi_mask[0] |= BIT_ULL(GICV5_ARCH_PPI_PMUIRQ);
}

static u32 vgic_v5_get_effective_priority_mask(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u32 highest_ap, priority_mask;

	/*
	 * Counting the number of trailing zeros gives the current active
	 * priority. Explicitly use the 32-bit version here as we have 32
	 * priorities. 32 then means that there are no active priorities.
	 */
	highest_ap = cpu_if->vgic_apr ? __builtin_ctz(cpu_if->vgic_apr) : 32;

	/*
	 * An interrupt is of sufficient priority if it is equal to or
	 * greater than the priority mask. Add 1 to the priority mask
	 * (i.e., lower priority) to match the APR logic before taking
	 * the min. This gives us the lowest priority that is masked.
	 */
	priority_mask = FIELD_GET(FEAT_GCIE_ICH_VMCR_EL2_VPMR, cpu_if->vgic_vmcr);

	return min(highest_ap, priority_mask + 1);
}

static bool vgic_v5_ppi_set_pending_state(struct kvm_vcpu *vcpu,
					  struct vgic_irq *irq)
{
	struct vgic_v5_cpu_if *cpu_if;
	const u32 id = FIELD_GET(GICV5_HWIRQ_ID, irq->intid);
	unsigned long *p;

	if (!vcpu || !irq)
		return false;

	/*
	 * For DVI'd interrupts, the state is directly driven by the host
	 * hardware connected to the interrupt line. There is nothing for us to
	 * do here. Moreover, this is just broken!
	 */
	if (WARN_ON(irq->directly_injected))
		return true;

	cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	p = (unsigned long *)&cpu_if->vgic_ppi_pendr[id / 64];
	__assign_bit(id % 64, p, irq_is_pending(irq));

	return true;
}

/*
 * For GICv5, the PPIs are mostly directly managed by the hardware. We (the
 * hypervisor) handle the pending, active, enable state save/restore, but don't
 * need the PPIs to be queued on a per-VCPU AP list. Therefore, sanity check the
 * state, unlock, and return.
 */
static bool vgic_v5_ppi_queue_irq_unlock(struct kvm *kvm, struct vgic_irq *irq,
					 unsigned long flags)
	__releases(&irq->irq_lock)
{
	struct kvm_vcpu *vcpu;

	lockdep_assert_held(&irq->irq_lock);

	if (WARN_ON_ONCE(!__irq_is_ppi(KVM_DEV_TYPE_ARM_VGIC_V5, irq->intid)))
		goto out_unlock_fail;

	vcpu = irq->target_vcpu;
	if (WARN_ON_ONCE(!vcpu))
		goto out_unlock_fail;

	raw_spin_unlock_irqrestore(&irq->irq_lock, flags);

	/* Directly kick the target VCPU to make sure it sees the IRQ */
	kvm_make_request(KVM_REQ_IRQ_PENDING, vcpu);
	kvm_vcpu_kick(vcpu);

	return true;

out_unlock_fail:
	raw_spin_unlock_irqrestore(&irq->irq_lock, flags);

	return false;
}

static struct irq_ops vgic_v5_ppi_irq_ops = {
	.set_pending_state = vgic_v5_ppi_set_pending_state,
	.queue_irq_unlock = vgic_v5_ppi_queue_irq_unlock,
};

void vgic_v5_set_ppi_ops(struct vgic_irq *irq)
{
	if (WARN_ON(!irq))
		return;

	guard(raw_spinlock_irqsave)(&irq->irq_lock);

	if (!WARN_ON(irq->ops))
		irq->ops = &vgic_v5_ppi_irq_ops;
}

/*
 * Sync back the PPI priorities to the vgic_irq shadow state for any interrupts
 * exposed to the guest (skipping all others).
 */
static void vgic_v5_sync_ppi_priorities(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u64 priorityr;

	/*
	 * We have 16 PPI Priority regs, but only have a few interrupts that the
	 * guest is allowed to use. Limit our sync of PPI priorities to those
	 * actually exposed to the guest by first iterating over the mask of
	 * exposed PPIs.
	 */
	for (int mask_reg = 0; mask_reg < 2; mask_reg++) {
		unsigned long *p;
		int i;

		p = (unsigned long *)&vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_mask[mask_reg];

		for_each_set_bit(i, p, 64) {
			struct vgic_irq *irq;
			int pri_idx, pri_reg;
			u32 intid;
			u8 priority;

			pri_reg = (mask_reg * 64 + i) / 8;
			pri_idx = (mask_reg * 64 + i) % 8;

			priorityr = cpu_if->vgic_ppi_priorityr[pri_reg];
			priority = (priorityr >> (pri_idx * 8)) & GENMASK(4, 0);

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, mask_reg * 64 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);

			scoped_guard(raw_spinlock_irqsave, &irq->irq_lock)
				irq->priority = priority;

			vgic_put_irq(vcpu->kvm, irq);
		}
	}
}

bool vgic_v5_has_pending_ppi(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	unsigned int priority_mask;

	/* If no pending bits are set, exit early */
	if (!cpu_if->vgic_ppi_pendr[0] && !cpu_if->vgic_ppi_pendr[1])
		return false;

	priority_mask = vgic_v5_get_effective_priority_mask(vcpu);

	/* If the combined priority mask is 0, nothing can be signalled! */
	if (!priority_mask)
		return false;

	for (int reg = 0; reg < 2; reg++) {
		const u64 enabler = cpu_if->vgic_ppi_enabler[reg];
		const u64 pendr = cpu_if->vgic_ppi_pendr[reg];
		unsigned long possible_bits;
		int i;

		/* Check all interrupts that are enabled and pending */
		possible_bits = enabler & pendr;

		/*
		 * Optimisation: pending and enabled with no active priorities
		 */
		if (possible_bits && priority_mask == 32)
			return true;

		for_each_set_bit(i, &possible_bits, 64) {
			bool has_pending = false;
			struct vgic_irq *irq;
			u32 intid;

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, reg * 64 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);

			scoped_guard(raw_spinlock_irqsave, &irq->irq_lock) {
				/*
				 * We know that the interrupt is
				 * enabled and pending, so only check
				 * the priority.
				 */
				if (irq->priority <= priority_mask)
					has_pending = true;
			}

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
void vgic_v5_fold_ppi_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	for (int reg = 0; reg < 2; reg++) {
		const u64 activer = host_data_ptr(vgic_v5_ppi_state)->activer_exit[reg];
		const u64 pendr = host_data_ptr(vgic_v5_ppi_state)->pendr_exit[reg];
		unsigned long changed_bits;
		int i;

		/*
		 * Track what changed across activer, pendr, but mask with
		 * ~DVI.
		 */
		changed_bits = cpu_if->vgic_ppi_activer[reg] ^ activer;
		changed_bits |= host_data_ptr(vgic_v5_ppi_state)->pendr_entry[reg] ^ pendr;
		changed_bits &= ~cpu_if->vgic_ppi_dvir[reg];

		for_each_set_bit(i, &changed_bits, 64) {
			struct vgic_irq *irq;
			u32 intid;

			intid = FIELD_PREP(GICV5_HWIRQ_TYPE, GICV5_HWIRQ_TYPE_PPI);
			intid |= FIELD_PREP(GICV5_HWIRQ_ID, reg * 64 + i);

			irq = vgic_get_vcpu_irq(vcpu, intid);

			scoped_guard(raw_spinlock_irqsave, &irq->irq_lock) {
				irq->active = !!(activer & BIT(i));

				/*
				 * This is an OR to avoid losing incoming
				 * edges!
				 */
				if (irq->config == VGIC_CONFIG_EDGE)
					irq->pending_latch |= !!(pendr & BIT(i));
			}

			vgic_put_irq(vcpu->kvm, irq);
		}

		/* Re-inject the exit state as entry state next time! */
		cpu_if->vgic_ppi_activer[reg] = activer;

		/*
		 * Pending state is a bit different. We only propagate back
		 * pending state for Edge interrupts. Moreover, this is OR'd
		 * with the incoming state to make sure we don't lose incoming
		 * edges. Use the (inverse) HMR to mask off all Level bits, and
		 * OR.
		 */
		cpu_if->vgic_ppi_pendr[reg] |=
			pendr & ~vcpu->kvm->arch.vgic.gicv5_vm.vgic_ppi_hmr[reg];
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
	host_data_ptr(vgic_v5_ppi_state)->pendr_entry[0] = cpu_if->vgic_ppi_pendr[0];
	host_data_ptr(vgic_v5_ppi_state)->pendr_entry[1] = cpu_if->vgic_ppi_pendr[1];

	/*
	 * Make sure that we can correctly detect "edges" in the PPI
	 * state. There's a path where we never actually enter the guest, and
	 * failure to do this risks losing pending state
	 */
	host_data_ptr(vgic_v5_ppi_state)->pendr_exit[0] = cpu_if->vgic_ppi_pendr[0];
	host_data_ptr(vgic_v5_ppi_state)->pendr_exit[1] = cpu_if->vgic_ppi_pendr[1];
}

/*
 * Sets/clears the corresponding bit in the ICH_PPI_DVIR register.
 */
int vgic_v5_set_ppi_dvi(struct kvm_vcpu *vcpu, u32 irq, bool dvi)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;
	u32 ppi = FIELD_GET(GICV5_HWIRQ_ID, irq);
	unsigned long *p;

	p = (unsigned long *)&cpu_if->vgic_ppi_dvir[ppi / 64];
	__assign_bit(ppi % 64, p, dvi);

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

	kvm_call_hyp(__vgic_v5_restore_vmcr_apr, cpu_if);

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

	kvm_call_hyp(__vgic_v5_save_apr, cpu_if);

	WRITE_ONCE(cpu_if->gicv5_vpe.resident, false);

	/* The shadow priority is only updated on entering WFI */
	if (vcpu_get_flag(vcpu, IN_WFI))
		vgic_v5_sync_ppi_priorities(vcpu);
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

	__vgic_v5_restore_state(cpu_if);
	kvm_call_hyp(__vgic_v5_restore_ppi_state, cpu_if);
	dsb(sy);
}

void vgic_v5_save_state(struct kvm_vcpu *vcpu)
{
	struct vgic_v5_cpu_if *cpu_if = &vcpu->arch.vgic_cpu.vgic_v5;

	__vgic_v5_save_state(cpu_if);
	kvm_call_hyp(__vgic_v5_save_ppi_state, cpu_if);
	dsb(sy);
}
