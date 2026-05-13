// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <uapi/linux/psci.h>
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

bool kvm_rmi_supports_sve(void)
{
	return rmi_has_feature(RMI_FEATURE_REGISTER_0_SVE);
}

u32 kvm_realm_ipa_limit(void)
{
	return u64_get_bits(rmm_feat_reg0, RMI_FEATURE_REGISTER_0_S2SZ);
}

u64 kvm_realm_reset_id_aa64dfr0_el1(const struct kvm_vcpu *vcpu, u64 val)
{
	u32 bps = u64_get_bits(rmm_feat_reg0, RMI_FEATURE_REGISTER_0_NUM_BPS);
	u32 wps = u64_get_bits(rmm_feat_reg0, RMI_FEATURE_REGISTER_0_NUM_WPS);
	u32 ctx_cmps;

	/* Ensure CTX_CMPs is still valid */
	ctx_cmps = FIELD_GET(ID_AA64DFR0_EL1_CTX_CMPs, val);
	ctx_cmps = min(bps, ctx_cmps);

	val &= ~(ID_AA64DFR0_EL1_BRPs_MASK | ID_AA64DFR0_EL1_WRPs_MASK |
		 ID_AA64DFR0_EL1_CTX_CMPs);
	val |= FIELD_PREP(ID_AA64DFR0_EL1_BRPs_MASK, bps) |
	       FIELD_PREP(ID_AA64DFR0_EL1_WRPs_MASK, wps) |
	       FIELD_PREP(ID_AA64DFR0_EL1_CTX_CMPs, ctx_cmps);

	return val;
}

static int get_start_level(struct realm *realm)
{
	return 4 - stage2_pgtable_levels(realm->ia_bits);
}

static int find_map_level(struct realm *realm,
			  unsigned long start,
			  unsigned long end)
{
	int level = KVM_PGTABLE_LAST_LEVEL;

	while (level > get_start_level(realm)) {
		unsigned long map_size = rmi_rtt_level_mapsize(level - 1);

		if (!IS_ALIGNED(start, map_size) ||
		    (start + map_size) > end)
			break;

		level--;
	}

	return level;
}

static unsigned long level_to_size(int level)
{
	switch (level) {
	case 0:
		return PAGE_SIZE;
	case 1:
		return PMD_SIZE;
	case 2:
		return PUD_SIZE;
	case 3:
		return P4D_SIZE;
	}
	WARN_ON(1);
	return 0;
}

static int undelegate_range_desc(unsigned long desc)
{
	unsigned long size = level_to_size(RMI_ADDR_RANGE_SIZE(desc));
	unsigned long count = RMI_ADDR_RANGE_COUNT(desc);
	unsigned long addr = RMI_ADDR_RANGE_ADDR(desc);
	unsigned long state = RMI_ADDR_RANGE_STATE(desc);

	if (state == RMI_OP_MEM_UNDELEGATED)
		return 0;

	if (size * count == 0)
		return 0;

	return rmi_undelegate_range(addr, size * count);
}

static phys_addr_t alloc_delegated_granule(struct kvm_mmu_memory_cache *mc)
{
	phys_addr_t phys;
	void *virt;

	if (mc) {
		virt = kvm_mmu_memory_cache_alloc(mc);
	} else {
		virt = (void *)__get_free_page(GFP_ATOMIC | __GFP_ZERO |
					       __GFP_ACCOUNT);
	}

	if (!virt)
		return PHYS_ADDR_MAX;

	phys = virt_to_phys(virt);
	if (rmi_delegate_page(phys)) {
		free_page((unsigned long)virt);
		return PHYS_ADDR_MAX;
	}

	return phys;
}

static phys_addr_t alloc_rtt(struct kvm_mmu_memory_cache *mc)
{
	phys_addr_t phys = alloc_delegated_granule(mc);

	if (phys != PHYS_ADDR_MAX)
		kvm_account_pgtable_pages(phys_to_virt(phys), 1);

	return phys;
}

static void free_rtt(phys_addr_t phys)
{
	if (free_delegated_page(phys))
		return;

	kvm_account_pgtable_pages(phys_to_virt(phys), -1);
}

int realm_psci_complete(struct kvm_vcpu *source, struct kvm_vcpu *target,
			unsigned long status)
{
	int ret;

	/*
	 * XXX: RMM-v2.0 doesn't require the target REC address for completing
	 * PSCI requests. Temporary hack until RMM implementation catches up
	 * to the full spec.
	 */
	ret = rmi_psci_complete(virt_to_phys(source->arch.rec.rec_page),
				virt_to_phys(target->arch.rec.rec_page),
				status);
	if (ret)
		return -EINVAL;

	return 0;
}

static int realm_rtt_create(struct realm *realm,
			    unsigned long addr,
			    int level,
			    phys_addr_t phys)
{
	addr = ALIGN_DOWN(addr, rmi_rtt_level_mapsize(level - 1));
	return rmi_rtt_create(virt_to_phys(realm->rd), phys, addr, level);
}

static int realm_rtt_fold(struct realm *realm,
			  unsigned long addr,
			  int level,
			  phys_addr_t *rtt_granule)
{
	unsigned long out_rtt;
	int ret;

	addr = ALIGN_DOWN(addr, rmi_rtt_level_mapsize(level - 1));
	ret = rmi_rtt_fold(virt_to_phys(realm->rd), addr, level, &out_rtt);

	if (rtt_granule)
		*rtt_granule = out_rtt;

	return ret;
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

static int realm_create_rtt_levels(struct realm *realm,
				   unsigned long ipa,
				   int level,
				   int max_level,
				   struct kvm_mmu_memory_cache *mc)
{
	while (level++ < max_level) {
		phys_addr_t rtt = alloc_rtt(mc);
		int ret;

		if (rtt == PHYS_ADDR_MAX)
			return -ENOMEM;

		ret = realm_rtt_create(realm, ipa, level, rtt);
		if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT &&
		    RMI_RETURN_INDEX(ret) == level - 1) {
			/* The RTT already exists, continue */
			free_rtt(rtt);
			continue;
		}

		if (ret) {
			WARN(1, "Failed to create RTT at level %d: %d\n",
			     level, ret);
			free_rtt(rtt);
			return -ENXIO;
		}
	}

	return 0;
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

/*
 * Returns 0 on successful fold, a negative value on error, a positive value if
 * we were not able to fold all tables at this level.
 */
static int realm_fold_rtt_level(struct realm *realm, int level,
				unsigned long start, unsigned long end)
{
	int not_folded = 0;
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

		ret = realm_rtt_fold(realm, align_addr, level, &rtt_granule);

		switch (RMI_RETURN_STATUS(ret)) {
		case RMI_SUCCESS:
			free_rtt(rtt_granule);
			break;
		case RMI_ERROR_RTT:
			if (level == KVM_PGTABLE_LAST_LEVEL ||
			    RMI_RETURN_INDEX(ret) < level) {
				not_folded++;
				break;
			}
			/* Recurse a level deeper */
			ret = realm_fold_rtt_level(realm,
						   level + 1,
						   addr,
						   next_addr);
			if (ret < 0) {
				return ret;
			} else if (ret == 0) {
				/* Try again at this level */
				next_addr = addr;
			}
			break;
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	return not_folded;
}

void kvm_realm_destroy_rtts(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned int ia_bits = realm->ia_bits;

	realm_tear_down_rtt_range(realm, 0, (1UL << ia_bits));
}

static void realm_unmap_shared_range(struct kvm *kvm,
				     unsigned long start,
				     unsigned long end,
				     bool may_block)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned long rd = virt_to_phys(realm->rd);
	unsigned long next_addr, addr;
	unsigned long shared_bit = BIT(realm->ia_bits - 1);

	start |= shared_bit;
	end |= shared_bit;

	for (addr = start; addr < end; addr = next_addr) {
		int ret;

		ret = rmi_rtt_unprot_unmap(rd, addr, end, RMI_ADDR_TYPE_NONE,
					   0, &next_addr, NULL, NULL);
		switch (RMI_RETURN_STATUS(ret)) {
		case RMI_SUCCESS:
			break;
		case RMI_ERROR_RTT: {
			int err_level = RMI_RETURN_INDEX(ret);
			int level = find_map_level(realm, addr, end);

			if (err_level >= level) {
				/* Nothing present, so skip */
				next_addr = addr + rmi_rtt_level_mapsize(err_level);
				break;
			}

			ret = realm_create_rtt_levels(realm, addr, err_level,
						      level, NULL);
			if (WARN_ON(ret))
				return;
			/* Retry with the RTT levels in place */
			next_addr = addr;
			break;
		}
		default:
			WARN_ON(1);
			return;
		}

		if (may_block)
			cond_resched_rwlock_write(&kvm->mmu_lock);
	}

	realm_fold_rtt_level(realm, get_start_level(realm) + 1,
			     start, end);
}

static int realm_create_rd(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	struct realm_params *params = realm->params;
	void *rd = NULL;
	phys_addr_t rd_phys, params_phys;
	size_t pgd_size = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);
	u64 dfr0 = kvm_read_vm_id_reg(kvm, SYS_ID_AA64DFR0_EL1);
	int r;

	realm->ia_bits = VTCR_EL2_IPA(kvm->arch.mmu.vtcr);

	if (WARN_ON(realm->rd || !realm->params))
		return -EEXIST;

	rd = (void *)__get_free_page(GFP_KERNEL_ACCOUNT);
	if (!rd)
		return -ENOMEM;

	rd_phys = virt_to_phys(rd);
	if (rmi_delegate_page(rd_phys)) {
		r = -ENXIO;
		goto free_rd;
	}

	if (rmi_delegate_range(kvm->arch.mmu.pgd_phys, pgd_size)) {
		r = -ENXIO;
		goto out_undelegate_tables;
	}

	params->s2sz = VTCR_EL2_IPA(kvm->arch.mmu.vtcr);
	params->rtt_level_start = get_start_level(realm);
	params->rtt_num_start = pgd_size / PAGE_SIZE;
	params->rtt_base = kvm->arch.mmu.pgd_phys;
	params->num_bps = SYS_FIELD_GET(ID_AA64DFR0_EL1, BRPs, dfr0);
	params->num_wps = SYS_FIELD_GET(ID_AA64DFR0_EL1, WRPs, dfr0);

	if (kvm->arch.arm_pmu) {
		params->pmu_num_ctrs = kvm->arch.nr_pmu_counters;
		params->flags |= RMI_REALM_PARAM_FLAG_PMU;
	}

	if (kvm_lpa2_is_enabled())
		params->flags |= RMI_REALM_PARAM_FLAG_LPA2;

	params_phys = virt_to_phys(params);

	if (rmi_realm_create(rd_phys, params_phys)) {
		r = -ENXIO;
		goto out_undelegate_tables;
	}

	realm->rd = rd;
	kvm_set_realm_state(kvm, REALM_STATE_NEW);
	/* The realm is up, free the parameters.  */
	free_page((unsigned long)realm->params);
	realm->params = NULL;

	return 0;

out_undelegate_tables:
	if (WARN_ON(rmi_undelegate_range(kvm->arch.mmu.pgd_phys, pgd_size))) {
		/* Leak the pages if they cannot be returned */
		kvm->arch.mmu.pgt = NULL;
	}
	if (WARN_ON(rmi_undelegate_page(rd_phys))) {
		/* Leak the page if it isn't returned */
		return r;
	}
free_rd:
	free_page((unsigned long)rd);
	return r;
}

static void realm_unmap_private_range(struct kvm *kvm,
				      unsigned long start,
				      unsigned long end,
				      bool may_block)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned long rd = virt_to_phys(realm->rd);
	unsigned long next_addr, addr;
	int ret;

	for (addr = start; addr < end; addr = next_addr) {
		unsigned long out_range;
		unsigned long flags = RMI_ADDR_TYPE_SINGLE;
		/* TODO: Optimise using RMI_ADDR_TYPE_LIST */

retry:
		ret = rmi_rtt_data_unmap(rd, addr, end, flags, 0,
					 &next_addr, &out_range, NULL);

		if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
			phys_addr_t rtt;

			if (next_addr > addr)
				continue; /* UNASSIGNED */

			rtt = alloc_rtt(NULL);
			if (WARN_ON(rtt == PHYS_ADDR_MAX))
				return;
			ret = realm_rtt_create(realm, addr,
					       RMI_RETURN_INDEX(ret) + 1, rtt);
			if (WARN_ON(ret)) {
				free_rtt(rtt);
				return;
			}
			goto retry;
		} else if (WARN_ON(ret)) {
			continue;
		}

		ret = undelegate_range_desc(out_range);
		if (WARN_ON(ret))
			break;

		if (may_block)
			cond_resched_rwlock_write(&kvm->mmu_lock);
	}

	realm_fold_rtt_level(realm, get_start_level(realm) + 1,
			     start, end);
}

void kvm_realm_unmap_range(struct kvm *kvm, unsigned long start,
			   unsigned long size, bool unmap_private,
			   bool may_block)
{
	unsigned long end = start + size;
	struct realm *realm = &kvm->arch.realm;

	if (!kvm_realm_is_created(kvm))
		return;

	end = min(BIT(realm->ia_bits - 1), end);

	realm_unmap_shared_range(kvm, start, end, may_block);
	if (unmap_private)
		realm_unmap_private_range(kvm, start, end, may_block);
}

static int realm_data_map_init(struct kvm *kvm, unsigned long ipa,
			       kvm_pfn_t dst_pfn, kvm_pfn_t src_pfn,
			       unsigned long flags)
{
	struct realm *realm = &kvm->arch.realm;
	phys_addr_t rd = virt_to_phys(realm->rd);
	phys_addr_t dst_phys, src_phys;
	int ret;

	dst_phys = __pfn_to_phys(dst_pfn);
	src_phys = __pfn_to_phys(src_pfn);

	if (rmi_delegate_page(dst_phys))
		return -ENXIO;

	ret = rmi_rtt_data_map_init(rd, dst_phys, ipa, src_phys, flags);
	if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
		/* Create missing RTTs and retry */
		int level = RMI_RETURN_INDEX(ret);

		KVM_BUG_ON(level == KVM_PGTABLE_LAST_LEVEL, kvm);

		ret = realm_create_rtt_levels(realm, ipa, level,
					      KVM_PGTABLE_LAST_LEVEL, NULL);
		if (!ret) {
			ret = rmi_rtt_data_map_init(rd, dst_phys, ipa, src_phys,
						    flags);
		}
	}

	if (ret) {
		if (WARN_ON(rmi_undelegate_page(dst_phys))) {
			/* Undelegate failed, so we leak the page */
			get_page(pfn_to_page(dst_pfn));
		}
	}

	return ret;
}

static unsigned long addr_range_desc(unsigned long phys, unsigned long size)
{
	unsigned long out = 0;

	switch (size) {
	case P4D_SIZE:
		out = 3 | (1 << 2);
		break;
	case PUD_SIZE:
		out = 2 | (1 << 2);
		break;
	case PMD_SIZE:
		out = 1 | (1 << 2);
		break;
	case PAGE_SIZE:
		out = 0 | (1 << 2);
		break;
	default:
		/*
		 * Only support mapping at the page level granulatity when
		 * it's an unusual length. This should get us back onto a larger
		 * block size for the subsequent mappings.
		 */
		out = 0 | ((MIN(size >> PAGE_SHIFT, PTRS_PER_PTE - 1)) << 2);
		break;
	}

	WARN_ON(phys & ~PAGE_MASK);

	out |= phys & PAGE_MASK;

	return out;
}

int realm_map_protected(struct kvm *kvm,
			unsigned long ipa,
			kvm_pfn_t pfn,
			unsigned long map_size,
			struct kvm_mmu_memory_cache *memcache)
{
	struct realm *realm = &kvm->arch.realm;
	phys_addr_t phys = __pfn_to_phys(pfn);
	phys_addr_t base_phys = phys;
	phys_addr_t rd = virt_to_phys(realm->rd);
	unsigned long base_ipa = ipa;
	unsigned long ipa_top = ipa + map_size;
	int ret = 0;

	if (WARN_ON(!IS_ALIGNED(map_size, PAGE_SIZE) ||
		    !IS_ALIGNED(ipa, map_size)))
		return -EINVAL;

	if (rmi_delegate_range(phys, map_size)) {
		/*
		 * It's likely we raced with another VCPU on the same
		 * fault. Assume the other VCPU has handled the fault
		 * and return to the guest.
		 */
		return 0;
	}

	while (ipa < ipa_top) {
		unsigned long flags = RMI_ADDR_TYPE_SINGLE;
		unsigned long range_desc = addr_range_desc(phys, ipa_top - ipa);
		unsigned long out_top;

		ret = rmi_rtt_data_map(rd, ipa, ipa_top, flags, range_desc,
				       &out_top);

		if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
			/* Create missing RTTs and retry */
			int level = RMI_RETURN_INDEX(ret);

			WARN_ON(level == KVM_PGTABLE_LAST_LEVEL);
			ret = realm_create_rtt_levels(realm, ipa, level,
						      KVM_PGTABLE_LAST_LEVEL,
						      memcache);
			if (ret)
				goto err_undelegate;

			ret = rmi_rtt_data_map(rd, ipa, ipa_top, flags,
					       range_desc, &out_top);
		}

		if (WARN_ON(ret))
			goto err_undelegate;

		phys += out_top - ipa;
		ipa = out_top;
	}

	return 0;

err_undelegate:
	realm_unmap_private_range(kvm, base_ipa, ipa, true);
	if (WARN_ON(rmi_undelegate_range(base_phys, map_size))) {
		/* Page can't be returned to NS world so is lost */
		get_page(phys_to_page(base_phys));
	}
	return -ENXIO;
}

int realm_map_non_secure(struct realm *realm,
			 unsigned long ipa,
			 kvm_pfn_t pfn,
			 unsigned long size,
			 enum kvm_pgtable_prot prot,
			 struct kvm_mmu_memory_cache *memcache)
{
	unsigned long attr, flags = 0;
	phys_addr_t rd = virt_to_phys(realm->rd);
	phys_addr_t phys = __pfn_to_phys(pfn);
	unsigned long ipa_top = ipa + size;
	int ret;

	if (WARN_ON(!IS_ALIGNED(size, PAGE_SIZE) ||
		    !IS_ALIGNED(ipa, size)))
		return -EINVAL;

	switch (prot & (KVM_PGTABLE_PROT_DEVICE | KVM_PGTABLE_PROT_NORMAL_NC)) {
	case KVM_PGTABLE_PROT_DEVICE | KVM_PGTABLE_PROT_NORMAL_NC:
		return -EINVAL;
	case KVM_PGTABLE_PROT_DEVICE:
		attr = MT_S2_FWB_DEVICE_nGnRE;
		break;
	case KVM_PGTABLE_PROT_NORMAL_NC:
		attr = MT_S2_FWB_NORMAL_NC;
		break;
	default:
		attr = MT_S2_FWB_NORMAL;
	}

	flags |= FIELD_PREP(RMI_RTT_UNPROT_MAP_FLAGS_MEMATTR, attr);

	if (prot & KVM_PGTABLE_PROT_R)
		flags |= FIELD_PREP(RMI_RTT_UNPROT_MAP_FLAGS_S2AP, RMI_S2AP_DIRECT_READ);
	if (prot & KVM_PGTABLE_PROT_W)
		flags |= FIELD_PREP(RMI_RTT_UNPROT_MAP_FLAGS_S2AP, RMI_S2AP_DIRECT_WRITE);

	flags |= RMI_ADDR_TYPE_SINGLE;

	while (ipa < ipa_top) {
		unsigned long range_desc = addr_range_desc(phys, ipa_top - ipa);
		unsigned long out_top;

		ret = rmi_rtt_unprot_map(rd, ipa, ipa_top, flags, range_desc,
					 &out_top);

		if (RMI_RETURN_STATUS(ret) == RMI_ERROR_RTT) {
			/* Create missing RTTs and retry */
			int level = RMI_RETURN_INDEX(ret);

			WARN_ON(level == KVM_PGTABLE_LAST_LEVEL);
			ret = realm_create_rtt_levels(realm, ipa, level,
						      KVM_PGTABLE_LAST_LEVEL,
						      memcache);
			if (ret)
				return ret;

			ret = rmi_rtt_unprot_map(rd, ipa, ipa_top, flags,
						 range_desc, &out_top);
		}

		if (WARN_ON(ret))
			return ret;

		phys += out_top - ipa;
		ipa = out_top;
	}

	return 0;
}

static int populate_region_cb(struct kvm *kvm, gfn_t gfn, kvm_pfn_t pfn,
			      struct page *src_page, void *opaque)
{
	unsigned long data_flags = *(unsigned long *)opaque;
	phys_addr_t ipa = gfn_to_gpa(gfn);

	if (!src_page)
		return -EOPNOTSUPP;

	return realm_data_map_init(kvm, ipa, pfn, page_to_pfn(src_page),
				   data_flags);
}

static long populate_region(struct kvm *kvm,
			    gfn_t base_gfn,
			    unsigned long pages,
			    u64 uaddr,
			    unsigned long data_flags)
{
	long ret = 0;

	mutex_lock(&kvm->slots_lock);
	ret = kvm_gmem_populate(kvm, base_gfn, u64_to_user_ptr(uaddr), pages,
				populate_region_cb, &data_flags);
	mutex_unlock(&kvm->slots_lock);

	return ret;
}

enum ripas_action {
	RIPAS_INIT,
	RIPAS_SET,
};

static int ripas_change(struct kvm *kvm,
			struct kvm_vcpu *vcpu,
			unsigned long ipa,
			unsigned long end,
			enum ripas_action action,
			unsigned long *top_ipa)
{
	struct realm *realm = &kvm->arch.realm;
	phys_addr_t rd_phys = virt_to_phys(realm->rd);
	phys_addr_t rec_phys;
	struct kvm_mmu_memory_cache *memcache = NULL;
	int ret = 0;

	if (vcpu) {
		rec_phys = virt_to_phys(vcpu->arch.rec.rec_page);
		memcache = &vcpu->arch.mmu_page_cache;

		WARN_ON(action != RIPAS_SET);
	} else {
		WARN_ON(action != RIPAS_INIT);
	}

	while (ipa < end) {
		unsigned long next = ~0;

		switch (action) {
		case RIPAS_INIT:
			ret = rmi_rtt_init_ripas(rd_phys, ipa, end, &next);
			break;
		case RIPAS_SET:
			ret = rmi_rtt_set_ripas(rd_phys, rec_phys, ipa, end,
						&next);
			break;
		}

		switch (RMI_RETURN_STATUS(ret)) {
		case RMI_SUCCESS:
			ipa = next;
			break;
		case RMI_ERROR_RTT: {
			int err_level = RMI_RETURN_INDEX(ret);
			int level = find_map_level(realm, ipa, end);

			ret = realm_create_rtt_levels(realm, ipa, err_level,
						      level, memcache);
			if (ret)
				return ret;
			/* Retry with the RTT levels in place */
			break;
		}
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	if (top_ipa)
		*top_ipa = ipa;

	return 0;
}

static int realm_set_ipa_state(struct kvm_vcpu *vcpu,
			       unsigned long start,
			       unsigned long end,
			       unsigned long ripas,
			       unsigned long *top_ipa)
{
	struct kvm *kvm = vcpu->kvm;
	int ret = ripas_change(kvm, vcpu, start, end, RIPAS_SET, top_ipa);

	if (ripas == RMI_EMPTY && *top_ipa != start)
		realm_unmap_private_range(kvm, start, *top_ipa, false);

	return ret;
}

static int realm_init_ipa_state(struct kvm *kvm,
				unsigned long gfn,
				unsigned long pages)
{
	return ripas_change(kvm, NULL, gfn_to_gpa(gfn), gfn_to_gpa(gfn + pages),
			    RIPAS_INIT, NULL);
}

static int realm_ensure_created(struct kvm *kvm)
{
	int ret;

	switch (kvm_realm_state(kvm)) {
	case REALM_STATE_NONE:
		break;
	case REALM_STATE_NEW:
		return 0;
	case REALM_STATE_DEAD:
		return -ENXIO;
	default:
		return -EBUSY;
	}

	ret = realm_create_rd(kvm);
	return ret;
}

static int set_ripas_of_protected_regions(struct kvm *kvm)
{
	struct kvm_memslots *slots;
	struct kvm_memory_slot *memslot;
	int idx, bkt;
	int ret = 0;

	idx = srcu_read_lock(&kvm->srcu);

	slots = kvm_memslots(kvm);
	kvm_for_each_memslot(memslot, bkt, slots) {
		if (!kvm_slot_has_gmem(memslot))
			continue;

		ret = realm_init_ipa_state(kvm, memslot->base_gfn,
					   memslot->npages);
		if (ret)
			break;
	}
	srcu_read_unlock(&kvm->srcu, idx);

	return ret;
}

int kvm_arm_rmi_populate(struct kvm *kvm,
			 struct kvm_arm_rmi_populate *args)
{
	unsigned long data_flags = 0;
	unsigned long ipa_start = args->base;
	unsigned long ipa_end = ipa_start + args->size;
	long pages_populated;
	int ret;

	if (args->reserved ||
	    (args->flags & ~KVM_ARM_RMI_POPULATE_FLAGS_MEASURE) ||
	    !IS_ALIGNED(ipa_start, PAGE_SIZE) ||
	    !IS_ALIGNED(ipa_end, PAGE_SIZE) ||
	    !IS_ALIGNED(args->source_uaddr, PAGE_SIZE))
		return -EINVAL;

	ret = realm_ensure_created(kvm);
	if (ret)
		return ret;

	if (args->flags & KVM_ARM_RMI_POPULATE_FLAGS_MEASURE)
		data_flags |= RMI_MEASURE_CONTENT;

	pages_populated = populate_region(kvm, gpa_to_gfn(ipa_start),
					  args->size >> PAGE_SHIFT,
					  args->source_uaddr, data_flags);

	if (pages_populated < 0)
		return pages_populated;

	args->size -= pages_populated << PAGE_SHIFT;
	args->source_uaddr += pages_populated << PAGE_SHIFT;
	args->base += pages_populated << PAGE_SHIFT;

	return 0;
}

static void kvm_complete_ripas_change(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct realm_rec *rec = &vcpu->arch.rec;
	unsigned long base = rec->run->exit.ripas_base;
	unsigned long top = rec->run->exit.ripas_top;
	unsigned long ripas = rec->run->exit.ripas_value;
	unsigned long top_ipa;
	int ret;

	do {
		kvm_mmu_topup_memory_cache(&vcpu->arch.mmu_page_cache,
					   kvm_mmu_cache_min_pages(vcpu->arch.hw_mmu));
		write_lock(&kvm->mmu_lock);
		ret = realm_set_ipa_state(vcpu, base, top, ripas, &top_ipa);
		write_unlock(&kvm->mmu_lock);

		if (WARN_RATELIMIT(ret && ret != -ENOMEM,
				   "Unable to satisfy RIPAS_CHANGE for %#lx - %#lx, ripas: %#lx\n",
				   base, top, ripas))
			break;

		base = top_ipa;
	} while (base < top);

	/*
	 * If this function is called again before the REC_ENTER call then
	 * avoid calling realm_set_ipa_state() again by changing to the value
	 * of ripas_base for the part that has already been covered. The RMM
	 * ignores the contains of the rec_exit structure so this doesn't
	 * affect the RMM.
	 */
	rec->run->exit.ripas_base = base;
}

static void kvm_rec_complete_psci(struct kvm_vcpu *vcpu)
{
	struct rec_run *run = vcpu->arch.rec.run;
	unsigned long status = PSCI_RET_DENIED;
	unsigned long ret = vcpu_get_reg(vcpu, 0);
	struct kvm_vcpu *target;

	switch (run->exit.gprs[0]) {
	/*
	 * XXX: RMM-v2.0 doesn't cause RMI_EXIT_PSCI for AFFINITY_INFO
	 * Temporary hack until tf-RMM gets the REC to MPIDR mapping via
	 * RD Auxiliary granules.
	 * For now always report SUCCESS
	 */
	case PSCI_0_2_FN64_AFFINITY_INFO:
		status = PSCI_RET_SUCCESS;
		break;
	case PSCI_0_2_FN64_CPU_ON: {
		if (ret != PSCI_RET_SUCCESS &&
		    ret != PSCI_RET_ALREADY_ON)
			status = PSCI_RET_DENIED;
		else
			status = PSCI_RET_SUCCESS;
		break;
	}
	default:
		return;
	}

	target = kvm_mpidr_to_vcpu(vcpu->kvm, run->exit.gprs[1]);
	/* RMM makes sure that we don't get RMI_EXIT_PSCI for invalid mpidrs */
	if (target)
		realm_psci_complete(vcpu, target, status);
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
	case RMI_EXIT_PSCI:
		kvm_rec_complete_psci(vcpu);
		break;
	case RMI_EXIT_RIPAS_CHANGE:
		kvm_complete_ripas_change(vcpu);
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

	ret = set_ripas_of_protected_regions(kvm);
	if (ret)
		return ret;

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
