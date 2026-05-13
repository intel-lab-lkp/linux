// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/kvm_host.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/rmi_cmds.h>
#include <asm/virt.h>

static inline unsigned long rmi_rtt_level_mapsize(int level)
{
	if (WARN_ON(level > KVM_PGTABLE_LAST_LEVEL))
		return PAGE_SIZE;

	return (1UL << ARM64_HW_PGTABLE_LEVEL_SHIFT(level));
}

static bool rmi_has_feature(unsigned long feature)
{
	return !!u64_get_bits(rmm_feat_reg0, feature);
}

u32 kvm_realm_ipa_limit(void)
{
	return u64_get_bits(rmm_feat_reg0, RMI_FEATURE_REGISTER_0_S2SZ);
}

static int get_start_level(struct realm *realm)
{
	return 4 - stage2_pgtable_levels(realm->ia_bits);
}

static void free_rtt(phys_addr_t phys)
{
	if (free_delegated_page(phys))
		return;

	kvm_account_pgtable_pages(phys_to_virt(phys), -1);
}

/*
 * realm_rtt_destroy - Destroy an RTT at @level for @addr.
 *
 * Returns - Result of the RMI_RTT_DESTROY call, and:
 * @rtt_granule:	RTT granule, if the RTT was destroyed.
 * @next_addr:		IPA corresponding to the next possible valid entry we
 *			can target
 */
static int realm_rtt_destroy(struct realm *realm, unsigned long addr,
			     int level, phys_addr_t *rtt_granule,
			     unsigned long *next_addr)
{
	unsigned long out_rtt;
	int ret;

	ret = rmi_rtt_destroy(virt_to_phys(realm->rd), addr, level,
			      &out_rtt, next_addr);

	*rtt_granule = out_rtt;

	return ret;
}

static int realm_tear_down_rtt_level(struct realm *realm, int level,
				     unsigned long start, unsigned long end)
{
	ssize_t map_size;
	unsigned long addr, next_addr;

	if (WARN_ON(level > KVM_PGTABLE_LAST_LEVEL))
		return -EINVAL;

	map_size = rmi_rtt_level_mapsize(level - 1);

	for (addr = start; addr < end; addr = next_addr) {
		phys_addr_t rtt_granule;
		int ret;
		unsigned long align_addr = ALIGN(addr, map_size);

		next_addr = ALIGN(addr + 1, map_size);

		if (next_addr > end || align_addr != addr) {
			/*
			 * The target range is smaller than what this level
			 * covers, recurse deeper.
			 */
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							min(next_addr, end));
			if (ret)
				return ret;
			continue;
		}

		ret = realm_rtt_destroy(realm, addr, level,
					&rtt_granule, &next_addr);

		switch (RMI_RETURN_STATUS(ret)) {
		case RMI_SUCCESS:
			free_rtt(rtt_granule);
			break;
		case RMI_ERROR_RTT:
			if (next_addr > addr) {
				/* Missing RTT, skip */
				break;
			}
			/*
			 * We tear down the RTT range for the full IPA
			 * space, after everything is unmapped. Also we
			 * descend down only if we cannot tear down a
			 * top level RTT. Thus RMM must be able to walk
			 * to the requested level. e.g., a block mapping
			 * exists at L1 or L2.
			 */
			if (WARN_ON(RMI_RETURN_INDEX(ret) != level))
				return -EBUSY;
			if (WARN_ON(level == KVM_PGTABLE_LAST_LEVEL))
				return -EBUSY;

			/*
			 * The table has active entries in it, recurse deeper
			 * and tear down the RTTs.
			 */
			next_addr = ALIGN(addr + 1, map_size);
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							next_addr);
			if (ret)
				return ret;
			/*
			 * Now that the child RTTs are destroyed,
			 * retry at this level.
			 */
			next_addr = addr;
			break;
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	return 0;
}

static int realm_tear_down_rtt_range(struct realm *realm,
				     unsigned long start, unsigned long end)
{
	/*
	 * Root level RTTs can only be destroyed after the RD is destroyed. So
	 * tear down everything below the root level
	 */
	return realm_tear_down_rtt_level(realm, get_start_level(realm) + 1,
					 start, end);
}

void kvm_realm_destroy_rtts(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned int ia_bits = realm->ia_bits;

	realm_tear_down_rtt_range(realm, 0, (1UL << ia_bits));
}

static int realm_ensure_created(struct kvm *kvm)
{
	/* Provided in later patch */
	return -ENXIO;
}

/*
 * kvm_rec_pre_enter - Complete operations before entering a REC
 *
 * Some operations require work to be completed before entering a realm. That
 * work may require memory allocation so cannot be done in the kvm_rec_enter()
 * call.
 *
 * Return: 1 if we should enter the guest
 *	   0 if we should exit to userspace
 *	   < 0 if we should exit to userspace, where the return value indicates
 *	   an error
 */
int kvm_rec_pre_enter(struct kvm_vcpu *vcpu)
{
	struct realm_rec *rec = &vcpu->arch.rec;

	if (kvm_realm_state(vcpu->kvm) != REALM_STATE_ACTIVE)
		return -EINVAL;

	switch (rec->run->exit.exit_reason) {
	case RMI_EXIT_HOST_CALL:
		for (int i = 0; i < REC_RUN_GPRS; i++)
			rec->run->enter.gprs[i] = vcpu_get_reg(vcpu, i);
		break;
	}

	return 1;
}

int noinstr kvm_rec_enter(struct kvm_vcpu *vcpu)
{
	struct realm_rec *rec = &vcpu->arch.rec;
	int ret;

	guest_state_enter_irqoff();
	ret = rmi_rec_enter(virt_to_phys(rec->rec_page),
			    virt_to_phys(rec->run));
	guest_state_exit_irqoff();

	return ret;
}

static int kvm_create_rec(struct kvm_vcpu *vcpu)
{
	struct user_pt_regs *vcpu_regs = vcpu_gp_regs(vcpu);
	unsigned long mpidr = kvm_vcpu_get_mpidr_aff(vcpu);
	struct realm *realm = &vcpu->kvm->arch.realm;
	struct realm_rec *rec = &vcpu->arch.rec;
	unsigned long rec_page_phys;
	struct rec_params *params;
	int r, i;

	if (rec->run)
		return -EBUSY;

	/*
	 * The RMM will report PSCI v1.0 to Realms and the KVM_ARM_VCPU_PSCI_0_2
	 * flag covers v0.2 and onwards.
	 */
	if (!vcpu_has_feature(vcpu, KVM_ARM_VCPU_PSCI_0_2))
		return -EINVAL;

	BUILD_BUG_ON(sizeof(*params) > PAGE_SIZE);
	BUILD_BUG_ON(sizeof(*rec->run) > PAGE_SIZE);

	params = (struct rec_params *)get_zeroed_page(GFP_KERNEL);
	rec->rec_page = (void *)__get_free_page(GFP_KERNEL);
	rec->run = (void *)get_zeroed_page(GFP_KERNEL);
	rec->sro = kmalloc_obj(*rec->sro);
	if (!params || !rec->rec_page || !rec->run || !rec->sro) {
		r = -ENOMEM;
		goto out_free_pages;
	}

	for (i = 0; i < ARRAY_SIZE(params->gprs); i++)
		params->gprs[i] = vcpu_regs->regs[i];

	params->pc = vcpu_regs->pc;

	if (vcpu->vcpu_id == 0)
		params->flags |= REC_PARAMS_FLAG_RUNNABLE;

	rec_page_phys = virt_to_phys(rec->rec_page);

	if (rmi_delegate_page(rec_page_phys)) {
		r = -ENXIO;
		goto out_free_pages;
	}

	params->mpidr = mpidr;

	if (rmi_rec_create(virt_to_phys(realm->rd), rec_page_phys,
			   virt_to_phys(params), rec->sro)) {
		r = -ENXIO;
		goto out_undelegate_rmm_rec;
	}

	rec->mpidr = mpidr;

	free_page((unsigned long)params);
	return 0;

out_undelegate_rmm_rec:
	if (WARN_ON(rmi_undelegate_page(rec_page_phys)))
		rec->rec_page = NULL;
out_free_pages:
	free_page((unsigned long)rec->run);
	free_page((unsigned long)rec->rec_page);
	free_page((unsigned long)params);
	kfree(rec->sro);
	rec->run = NULL;
	return r;
}

void kvm_destroy_rec(struct kvm_vcpu *vcpu)
{
	struct realm_rec *rec = &vcpu->arch.rec;
	unsigned long rec_page_phys;

	if (!vcpu_is_rec(vcpu))
		return;

	if (!rec->run) {
		/* Nothing to do if the VCPU hasn't been finalized */
		return;
	}

	free_page((unsigned long)rec->run);

	rec_page_phys = virt_to_phys(rec->rec_page);

	if (WARN_ON(rmi_rec_destroy(rec_page_phys, rec->sro)))
		return;

	kfree(rec->sro);

	free_delegated_page(rec_page_phys);
}

int kvm_activate_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	int ret;

	if (kvm_realm_state(kvm) >= REALM_STATE_ACTIVE)
		return 0;

	if (!irqchip_in_kernel(kvm)) {
		/* Userspace irqchip not yet supported with realms */
		return -EOPNOTSUPP;
	}

	guard(mutex)(&kvm->arch.config_lock);
	/* Check again with the lock held */
	if (kvm_realm_state(kvm) >= REALM_STATE_ACTIVE)
		return 0;

	ret = realm_ensure_created(kvm);
	if (ret)
		return ret;

	/* Mark state as dead in case we fail */
	kvm_set_realm_state(kvm, REALM_STATE_DEAD);

	kvm_for_each_vcpu(i, vcpu, kvm) {
		ret = kvm_create_rec(vcpu);
		if (ret)
			return ret;
	}

	ret = rmi_realm_activate(virt_to_phys(realm->rd));
	if (ret)
		return -ENXIO;

	kvm_set_realm_state(kvm, REALM_STATE_ACTIVE);
	return 0;
}

void kvm_destroy_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	size_t pgd_size = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	if (realm->params) {
		free_page((unsigned long)realm->params);
		realm->params = NULL;
	}

	if (!kvm_realm_is_created(kvm))
		return;

	kvm_set_realm_state(kvm, REALM_STATE_DYING);

	write_lock(&kvm->mmu_lock);
	kvm_stage2_unmap_range(&kvm->arch.mmu, 0,
			       BIT(realm->ia_bits - 1), true);
	write_unlock(&kvm->mmu_lock);

	if (realm->rd) {
		phys_addr_t rd_phys = virt_to_phys(realm->rd);

		if (WARN_ON(rmi_realm_terminate(rd_phys)))
			return;

		kvm_realm_destroy_rtts(kvm);

		if (WARN_ON(rmi_realm_destroy(rd_phys)))
			return;
		free_delegated_page(rd_phys);
		realm->rd = NULL;
	}

	if (WARN_ON(rmi_undelegate_range(kvm->arch.mmu.pgd_phys, pgd_size)))
		return;

	kvm_set_realm_state(kvm, REALM_STATE_DEAD);

	/* Now that the Realm is destroyed, free the entry level RTTs */
	kvm_free_stage2_pgd(&kvm->arch.mmu);
}

int kvm_init_realm(struct kvm *kvm)
{
	kvm->arch.realm.params = (void *)get_zeroed_page(GFP_KERNEL_ACCOUNT);

	if (!kvm->arch.realm.params)
		return -ENOMEM;
	return 0;
}

static int rmm_check_features(void)
{
	if (kvm_lpa2_is_enabled() && !rmi_has_feature(RMI_FEATURE_REGISTER_0_LPA2)) {
		kvm_err("RMM doesn't support LPA2");
		return -ENXIO;
	}

	return 0;
}

void kvm_init_rmi(void)
{
	/*
	 * TODO: Support Realm guests in nVHE mode, this will require adding
	 * EL2 stub(s) for REC entry and possibly other things.
	 */
	if (!is_kernel_in_hyp_mode())
		return;

	if (!rmi_is_available())
		return;

	if (rmm_check_features())
		return;

	/* Future patch will enable static branch kvm_rmi_is_available */
}
