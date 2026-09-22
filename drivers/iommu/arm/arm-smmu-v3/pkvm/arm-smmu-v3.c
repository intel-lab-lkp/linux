// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/trap_handler.h>

#include "arm-smmu-v3-hyp.h"
#include "../arm-smmu-v3.h"
#include "../arm-smmu-v3-common-lib.h"

#include <linux/io-pgtable.h>
#include "../../../io-pgtable-arm.h"

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

/* strtab accessors */
#define strtab_log2size(smmu)	(FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, (smmu)->host_ste_cfg))
#define strtab_size(smmu)	((1UL << strtab_log2size(smmu)) * STRTAB_STE_DWORDS * 8)
#define strtab_host_base(smmu)	((smmu)->host_ste_base & STRTAB_BASE_ADDR_MASK)
#define strtab_split(smmu)	(FIELD_GET(STRTAB_BASE_CFG_SPLIT, (smmu)->host_ste_cfg))
#define strtab_l1_size(smmu)	((1UL << (strtab_log2size(smmu) - strtab_split(smmu))) * \
				 (sizeof(struct arm_smmu_strtab_l1)))
#define strtab_hyp_base(smmu)	((smmu)->features & ARM_SMMU_FEAT_2_LVL_STRTAB ? \
				 (u64 *)(smmu)->strtab_cfg.l2.l1tab :\
				 (u64 *)(smmu)->strtab_cfg.linear.table)

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

#define cmdq_size(cmdq)	((1 << ((cmdq)->llq.max_n_shift)) * CMDQ_ENT_DWORDS * 8)

static u32 timer_freq;

static inline u64 hyp_clock_ns(void)
{
	return mul_u64_u32_div(__arch_counter_get_cntvct(), NSEC_PER_SEC, timer_freq);
}

#define ARM_SMMU_EL2_POLL_TIMEOUT_US	1000
/*
 * Wait until @cond is true, can use WFE, if polling on an SMMU and
 * event that supports it.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(__use_wfe, __cond)					\
({									\
	int __ret = 0;							\
	u64 __start = hyp_clock_ns();					\
	u64 __timeout = ARM_SMMU_EL2_POLL_TIMEOUT_US * NSEC_PER_USEC;	\
									\
	while (!(__cond)) {						\
		if (__use_wfe) {					\
			wfe();						\
		} else {						\
			cpu_relax();					\
		}							\
		if ((hyp_clock_ns() - __start) >= __timeout) {		\
			if (__cond)					\
				break;					\
			__ret = -ETIMEDOUT;				\
			break;						\
		}							\
	}								\
	__ret;								\
})

/* Protected by host_mmu.lock from core code. */
static struct io_pgtable *idmap_pgtable;

static bool is_cmdq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_CMDQEN, smmu->cr0);
}

static bool is_smmu_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_SMMUEN, smmu->cr0);
}

static bool is_evtq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_EVTQEN, smmu->cr0);
}

static bool is_priq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_PRIQEN, smmu->cr0);
}

/*
 * CMDQ, STE host copies are accessed by the hypervisor, we share them to
 * - Prevent the host from passing protected VM memory.
 * - Having them mapped in the hyp page table.
 */
static int smmu_share_pages(phys_addr_t addr, size_t size)
{
	size_t nr_pages = PAGE_ALIGN(size + (addr & ~PAGE_MASK)) >> PAGE_SHIFT;
	phys_addr_t base = addr & PAGE_MASK;
	int i, ret;

	for (i = 0; i < nr_pages; ++i) {
		if (__pkvm_host_share_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT)) {
			while (i--)
				__pkvm_host_unshare_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT);
			return -EPERM;
		}
	}

	ret = hyp_pin_shared_mem(hyp_phys_to_virt(base),
				 hyp_phys_to_virt(base + nr_pages * PAGE_SIZE));
	if (ret) {
		for (i = 0; i < nr_pages; ++i)
			__pkvm_host_unshare_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT);
	}

	return ret;
}

static int smmu_unshare_pages(phys_addr_t addr, size_t size)
{
	size_t nr_pages = PAGE_ALIGN(size + (addr & ~PAGE_MASK)) >> PAGE_SHIFT;
	phys_addr_t base = addr & PAGE_MASK;
	int i, ret;

	hyp_unpin_shared_mem(hyp_phys_to_virt(base),
			     hyp_phys_to_virt(base + nr_pages * PAGE_SIZE));

	for (i = 0; i < nr_pages; ++i) {
		ret = __pkvm_host_unshare_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT);
		if (ret)
			return ret;
	}

	return 0;
}

static bool smmu_cmdq_has_space(struct arm_smmu_queue *cmdq, u32 n)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_has_space(llq, n);
}

static bool smmu_cmdq_full(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_full(llq);
}

static bool smmu_cmdq_empty(struct arm_smmu_queue *cmdq)
{
	struct arm_smmu_ll_queue *llq = &cmdq->llq;

	WRITE_ONCE(llq->cons, readl_relaxed(cmdq->cons_reg));
	return queue_empty(llq);
}

static void smmu_add_cmd_raw(struct hyp_arm_smmu_v3_device *smmu,
			     u64 *cmd)
{
	struct arm_smmu_queue *q = &smmu->cmdq;
	struct arm_smmu_ll_queue *llq = &q->llq;

	queue_write(Q_ENT(q, llq->prod), cmd,  CMDQ_ENT_DWORDS);
	llq->prod = queue_inc_prod_n(llq, 1);
}

static int smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu,
			struct arm_smmu_cmd *cmd)
{
	int ret;

	hyp_assert_lock_held(&smmu->hw_lock);
	ret = smmu_wait(false, !smmu_cmdq_full(&smmu->cmdq));
	if (ret)
		return ret;

	smmu_add_cmd_raw(smmu, cmd->data);
	writel(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmd cmd;

	hyp_assert_lock_held(&smmu->hw_lock);
	cmd = arm_smmu_make_cmd_sync(CMDQ_SYNC_0_CS_SEV, 0);
	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait(smmu->features & ARM_SMMU_FEAT_SEV,
			 smmu_cmdq_empty(&smmu->cmdq));
}

static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmd *cmd)
{
	int ret;

	hyp_assert_lock_held(&smmu->hw_lock);
	ret = smmu_add_cmd(smmu, cmd);
	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
}

static int smmu_tlb_inv_vmid(struct hyp_arm_smmu_v3_device *smmu)
{
	struct arm_smmu_cmd cmd = arm_smmu_make_cmd_op(CMDQ_OP_TLBI_S12_VMALL);

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_tlb_range_inv_cmd(struct hyp_arm_smmu_v3_device *smmu,
				  struct arm_smmu_cmd *cmd,
				  unsigned long start, unsigned int num,
				  unsigned int scale, u8 ttl, bool leaf)
{
	size_t tg = __ffs(idmap_pgtable->cfg.pgsize_bitmap);
	u8 tg_enc = arm_smmu_tlb_inv_tg_enc(tg);

	cmd->data[0] |= arm_smmu_tlb_inv_range_enc(num, scale);
	cmd->data[1] = arm_smmu_tlb_inv_addr(start, leaf, ttl, tg_enc);
	return smmu_add_cmd(smmu, cmd);
}

static int __smmu_tlb_inv_range(struct hyp_arm_smmu_v3_device *smmu,
				struct arm_smmu_cmd *cmd,
				unsigned long iova, size_t size, size_t granule,
				bool leaf)
{
	size_t tg = __ffs(idmap_pgtable->cfg.pgsize_bitmap);
	unsigned long n = size >> tg;
	unsigned long second_start;
	u64 data0 = cmd->data[0];
	unsigned int num, scale;
	u8 ttl;
	int ret;

	/* Only leaf invalidations know the level, non-leaf must use TTL=0. */
	ttl = leaf ? arm_smmu_tlb_inv_ttl(granule, tg) : 0;
	scale = fls64((n - 1) / 32);
	/* Scale is up to 5 bits. */
	if (scale > 31)
		return smmu_tlb_inv_vmid(smmu);

	num = n >> scale;
	ret = smmu_tlb_range_inv_cmd(smmu, cmd, iova, num, scale, ttl, leaf);
	if (ret)
		return ret;

	n -= (unsigned long)num << scale;
	if (n) {
		scale = fls64((n - 1) / 32);
		num = DIV_ROUND_UP(n, 1UL << scale);
		second_start = iova + size - ((unsigned long)num << (scale + tg));

		cmd->data[0] = data0;
		ret = smmu_tlb_range_inv_cmd(smmu, cmd, second_start, num, scale, 0, leaf);
		if (ret)
			return ret;
	}

	return smmu_sync_cmd(smmu);
}

static int __smmu_tlb_inv_range_pages(struct hyp_arm_smmu_v3_device *smmu,
				      struct arm_smmu_cmd *cmd,
				      unsigned long iova, size_t size, size_t granule,
				      bool leaf)
{
	unsigned long end = iova + size;
	int ret;

	/* See arm_smmu_inv_size_too_big() */
	if (size >= (1UL << (ilog2(granule) - 3)) * granule)
		return smmu_tlb_inv_vmid(smmu);

	for (; iova < end; iova += granule) {
		cmd->data[1] = arm_smmu_tlb_inv_addr(iova, leaf, 0, 0);
		ret = smmu_add_cmd(smmu, cmd);
		if (ret)
			return ret;
	}

	return smmu_sync_cmd(smmu);
}

static int smmu_tlb_inv_range_smmu(struct hyp_arm_smmu_v3_device *smmu,
				   unsigned long iova, size_t size, size_t granule,
				   bool leaf)
{
	struct arm_smmu_cmd cmd_s1 = arm_smmu_make_cmd_op(CMDQ_OP_TLBI_NH_ALL);
	struct arm_smmu_cmd cmd = arm_smmu_make_cmd_op(CMDQ_OP_TLBI_S2_IPA);
	int ret;

	if (smmu->features & ARM_SMMU_FEAT_RANGE_INV)
		ret = __smmu_tlb_inv_range(smmu, &cmd, iova, size, granule, leaf);
	else
		ret = __smmu_tlb_inv_range_pages(smmu, &cmd, iova, size, granule, leaf);
	if (ret)
		return ret;

	return smmu_send_cmd(smmu, &cmd_s1);
}

static void smmu_tlb_inv_range(unsigned long iova, size_t size, size_t granule,
			       bool leaf)
{
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu) {
		hyp_spin_lock(&smmu->hw_lock);
		/*
		 * Don't bother if CMDQ is disabled, this would be useful for the case
		 * when RPM is supported to avoid touching the SMMU MMIO when disabled.
		 * The hypervisor also asserts CMDQEN is enabled before the SMMU is
		 * enabled. As otherwise the host can prevent the hypervisor from doing
		 * TLB invalidations.
		 * When the SMMU is re-enabled the hypervisor cleans the TLBs.
		 */
		if (smmu->cmdq_active)
			WARN_ON(smmu_tlb_inv_range_smmu(smmu, iova, size,
							granule, leaf));
		hyp_spin_unlock(&smmu->hw_lock);
	}
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	smmu_tlb_inv_range(iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	smmu_tlb_inv_range(iova, granule, granule, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

static int smmu_abort_gbpa(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	u32 reg;

	ret = smmu_wait(false,
			(readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_UPDATE) == 0);
	if (ret)
		return ret;

	reg = readl_relaxed(smmu->base + ARM_SMMU_GBPA);
	writel_relaxed(GBPA_UPDATE | GBPA_ABORT | reg, smmu->base + ARM_SMMU_GBPA);
	return smmu_wait(false,
			 (readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_UPDATE) == 0);
}

/* Put the device in a state that can be probed by the host driver. */
static void smmu_deinit_device(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(__pkvm_hyp_donate_host_mmio(hyp_phys_to_pfn(smmu->mmio_addr),
					    smmu->mmio_size >> PAGE_SHIFT));
	if (smmu->cmdq.base)
		WARN_ON(__pkvm_hyp_donate_host(smmu->cmdq.base_dma >> PAGE_SHIFT,
					       PAGE_ALIGN(cmdq_size(&smmu->cmdq)) >> PAGE_SHIFT));

	if (smmu->strtab_cfg.linear.table ||
	    smmu->strtab_cfg.l2.l1tab)
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(smmu->strtab_dma),
					       smmu->strtab_size >> PAGE_SHIFT));
	smmu->base = NULL;
}

/*
 * Mini-probe and validation for the hypervisor.
 */
static int smmu_probe(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 reg;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		return -EINVAL;

	reg = arm_smmu_idr0_probe(smmu);

	if (!(FIELD_GET(IDR0_TTF, reg) & IDR0_TTF_AARCH64))
		return -ENXIO;

	if (!(smmu->features & (ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE)))
		return -ENXIO;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL))
		return -EINVAL;

	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);
	smmu->cmdq_max_shift = FIELD_GET(IDR1_CMDQS, reg);
	/* Follows the kernel logic */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	arm_smmu_idr3_probe(smmu);

	arm_smmu_idr5_probe(smmu);
	if (!smmu->oas)
		smmu->oas = 48;

	arm_smmu_device_iidr_probe(smmu);

	if (!(smmu->features & ARM_SMMU_FEAT_NESTING))
		return -ENXIO;

	return 0;
}

/*
 * The kernel part of the driver will allocate the shadow cmdq,
 * and zero it. This function only donates it.
 */
static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t cmdq_nr_pages;
	int ret;

	smmu->cmdq.llq.max_n_shift = min(smmu->cmdq.llq.max_n_shift, smmu->cmdq_max_shift);
	cmdq_nr_pages = PAGE_ALIGN(cmdq_size(&smmu->cmdq)) >> PAGE_SHIFT;
	ret = __pkvm_host_donate_hyp(smmu->cmdq.base_dma >> PAGE_SHIFT, cmdq_nr_pages);
	if (ret)
		return ret;

	smmu->cmdq.base = hyp_phys_to_virt(smmu->cmdq.base_dma);
	smmu->cmdq.prod_reg = smmu->base + ARM_SMMU_CMDQ_PROD;
	smmu->cmdq.cons_reg = smmu->base + ARM_SMMU_CMDQ_CONS;
	smmu->cmdq.q_base = smmu->cmdq.base_dma |
			    FIELD_PREP(Q_BASE_LOG2SIZE, smmu->cmdq.llq.max_n_shift);
	smmu->cmdq.ent_dwords = CMDQ_ENT_DWORDS;
	writel_relaxed(0, smmu->cmdq.prod_reg);
	writel_relaxed(0, smmu->cmdq.cons_reg);
	writeq_relaxed(smmu->cmdq.q_base, smmu->base + ARM_SMMU_CMDQ_BASE);
	return 0;
}

static int smmu_get_host_l2_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid,
				struct arm_smmu_ste *host_ste_out)
{
	u64 *host_ste_base = hyp_phys_to_virt(strtab_host_base(smmu));
	struct arm_smmu_strtab_l1 host_l1_desc;
	struct arm_smmu_strtab_l2 *l2ptr;
	phys_addr_t host_l2_tab;
	int ret, i;

	host_l1_desc.l2ptr = READ_ONCE(host_ste_base[arm_smmu_strtab_l1_idx(sid)]);
	if (!(le64_to_cpu(host_l1_desc.l2ptr) & STRTAB_L1_DESC_SPAN))
		return -EINVAL;

	host_l2_tab = le64_to_cpu(host_l1_desc.l2ptr) & STRTAB_L1_DESC_L2PTR_MASK;
	/* Share and pin the table before accessing it. */
	ret = smmu_share_pages(host_l2_tab, sizeof(struct arm_smmu_strtab_l2));
	if (ret)
		return ret;

	l2ptr = hyp_phys_to_virt(host_l2_tab);

	for (i = 0; i < STRTAB_STE_DWORDS; ++i)
		host_ste_out->data[i] =
			READ_ONCE(l2ptr->stes[arm_smmu_strtab_l2_idx(sid)].data[i]);

	WARN_ON(smmu_unshare_pages(host_l2_tab, sizeof(struct arm_smmu_strtab_l2)));
	return 0;
}

static int smmu_reshadow_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid, bool leaf)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_ste *hyp_ste_ptr, *host_ste_ptr, host_ste_copy;
	u64 *hyp_ste_base = strtab_hyp_base(smmu);
	int ret, i;

	/* Leaf = 0 is not supported, as the kernel driver. */
	if (!leaf || !is_smmu_enabled(smmu) ||
	    (sid >= (1UL << strtab_log2size(smmu))))
		return -EINVAL;

	if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)) {
		struct arm_smmu_ste *hyp_table = (struct arm_smmu_ste *)hyp_ste_base;
		u64 *host_ste_base = hyp_phys_to_virt(strtab_host_base(smmu));
		struct arm_smmu_ste *host_table = (struct arm_smmu_ste *)host_ste_base;

		if (sid >= cfg->linear.num_ents)
			return -E2BIG;

		hyp_ste_ptr = &hyp_table[sid];
		host_ste_ptr = &host_table[sid];
	} else {
		struct arm_smmu_strtab_l1 *l1tab = (struct arm_smmu_strtab_l1 *)hyp_ste_base;
		u32 l1_idx = arm_smmu_strtab_l1_idx(sid);
		struct arm_smmu_strtab_l2 *l2ptr;

		if (l1_idx >= cfg->l2.num_l1_ents)
			return -E2BIG;

		host_ste_ptr = &host_ste_copy;
		ret = smmu_get_host_l2_ste(smmu, sid, host_ste_ptr);
		if (ret)
			return ret;

		/* No hypervisor entry, first time the L2 is populated. */
		if (!l1tab[l1_idx].l2ptr) {
			struct arm_smmu_strtab_l2 *l2table;

			l2table = pkvm_iommu_alloc_pages(get_order(sizeof(*l2table)));
			if (!l2table)
				return -ENOMEM;
			/* Ensure L2 is observed before writing the ptr. */
			dma_wmb();
			arm_smmu_write_strtab_l1_desc(&l1tab[l1_idx], hyp_virt_to_phys(l2table));
		}
		l2ptr = hyp_phys_to_virt(le64_to_cpu(l1tab[l1_idx].l2ptr) &
				STRTAB_L1_DESC_L2PTR_MASK);
		hyp_ste_ptr = &l2ptr->stes[arm_smmu_strtab_l2_idx(sid)];
	}

	for (i = 0; i < STRTAB_STE_DWORDS; ++i)
		WRITE_ONCE(hyp_ste_ptr->data[i], host_ste_ptr->data[i]);
	return 0;
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	int ret;
	u32 reg;

	ret = __pkvm_host_donate_hyp(hyp_phys_to_pfn(smmu->strtab_dma),
				     smmu->strtab_size >> PAGE_SHIFT);
	if (ret)
		return ret;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		unsigned int last_sid_idx =
			arm_smmu_strtab_l1_idx((1ULL << smmu->sid_bits) - 1);

		cfg->l2.l1tab = hyp_phys_to_virt(smmu->strtab_dma);
		cfg->l2.l1_dma = smmu->strtab_dma;
		cfg->l2.num_l1_ents = min(last_sid_idx + 1, STRTAB_MAX_L1_ENTRIES);

		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_2LVL) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE,
				 ilog2(cfg->l2.num_l1_ents) + STRTAB_SPLIT) |
		      FIELD_PREP(STRTAB_BASE_CFG_SPLIT, STRTAB_SPLIT);
	} else {
		cfg->linear.table = hyp_phys_to_virt(smmu->strtab_dma);
		cfg->linear.ste_dma = smmu->strtab_dma;
		cfg->linear.num_ents = 1UL << smmu->sid_bits;
		reg = FIELD_PREP(STRTAB_BASE_CFG_FMT,
				 STRTAB_BASE_CFG_FMT_LINEAR) |
		      FIELD_PREP(STRTAB_BASE_CFG_LOG2SIZE, smmu->sid_bits);
	}

	writeq_relaxed((smmu->strtab_dma & STRTAB_BASE_ADDR_MASK) | STRTAB_BASE_RA,
		       smmu->base + ARM_SMMU_STRTAB_BASE);
	writel_relaxed(reg, smmu->base + ARM_SMMU_STRTAB_BASE_CFG);
	return 0;
}

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	unsigned long haddr;
	int ret;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	hyp_spin_lock_init(&smmu->lock);
	hyp_spin_lock_init(&smmu->hw_lock);

	ret = __pkvm_host_donate_hyp_mmio(hyp_phys_to_pfn(smmu->mmio_addr),
					  smmu->mmio_size >> PAGE_SHIFT, &haddr);
	if (ret)
		return ret;

	smmu->base = (void __iomem *)haddr;
	ret = smmu_probe(smmu);
	if (ret)
		goto out_ret;

	ret = smmu_init_cmdq(smmu);
	if (ret)
		goto out_ret;

	ret = smmu_init_strtab(smmu);
	if (ret)
		goto out_ret;

	ret = smmu_abort_gbpa(smmu);
	if (ret)
		goto out_ret;

	return 0;

out_ret:
	smmu_deinit_device(smmu);
	return ret;
}

static int smmu_init_pgt(void)
{
	/* Default values overridden based on SMMUs common features. */
	struct io_pgtable_cfg cfg = (struct io_pgtable_cfg) {
		.tlb = &smmu_tlb_ops,
		.pgsize_bitmap = ~0UL,
		.ias = 48,
		.oas = 48,
		.coherent_walk = true,
		.quirks = IO_PGTABLE_QUIRK_NO_WARN,
	};
	struct hyp_arm_smmu_v3_device *smmu;
	struct io_pgtable_ops *ops;

	for_each_smmu(smmu) {
		cfg.ias = min(cfg.ias, smmu->oas);
		cfg.oas = min(cfg.oas, smmu->oas);
		cfg.pgsize_bitmap &= smmu->pgsize_bitmap;
		cfg.coherent_walk &= !!(smmu->features & ARM_SMMU_FEAT_COHERENCY);
	}

	/* At least PAGE_SIZE must be supported by all SMMUs */
	if ((cfg.pgsize_bitmap & PAGE_SIZE) == 0)
		return -EINVAL;

	ops = kvm_alloc_io_pgtable_ops(ARM_64_LPAE_S2, &cfg, NULL);
	if (!ops)
		return -ENOMEM;
	idmap_pgtable = io_pgtable_ops_to_pgtable(ops);
	return 0;
}

/* Called while is the host is still trusted. */
static int smmu_init(void)
{
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) *
					  kvm_hyp_arm_smmu_v3_count);
	struct hyp_arm_smmu_v3_device *smmu;
	u64 pfn, nr_pages;
	int ret;

	timer_freq = read_sysreg(cntfrq_el0);
	if (!timer_freq || timer_freq > NSEC_PER_SEC)
		return -EINVAL;

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);
	pfn = hyp_virt_to_pfn(kvm_hyp_arm_smmu_v3_smmus);
	nr_pages = smmu_arr_size >> PAGE_SHIFT;

	ret = __pkvm_host_donate_hyp(pfn, nr_pages);
	if (ret)
		return ret;

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			goto out_reclaim_smmu;
	}

	BUILD_BUG_ON(sizeof(hyp_spinlock_t) != sizeof(u32));

	ret = smmu_init_pgt();
	if (ret)
		goto out_reclaim_smmu;
	return ret;

out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	WARN_ON(__pkvm_hyp_donate_host(pfn, nr_pages));
	return ret;
}

static bool smmu_filter_command(struct hyp_arm_smmu_v3_device *smmu, u64 *command)
{
	u64 type = FIELD_GET(CMDQ_0_OP, command[0]);

	switch (type) {
	case CMDQ_OP_CFGI_STE:
	{
		u32 leaf = FIELD_GET(CMDQ_CFGI_1_LEAF, command[1]);
		u32 sid = FIELD_GET(CMDQ_CFGI_0_SID, command[0]);
		int ret;

		/*
		 * If STE update is required, flush the CMDQ and drop the lock as that
		 * might require to update the host page table and acquire its lock.
		 */
		writel(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);
		hyp_spin_unlock(&smmu->hw_lock);
		ret = smmu_reshadow_ste(smmu, sid, leaf);
		hyp_spin_lock(&smmu->hw_lock);
		if (ret)
			return true;

		break;
	}
	case CMDQ_OP_CFGI_ALL:
	{
		/*
		 * Linux doesn't use range STE invalidation, and only use this
		 * for CFGI_ALL, which is done on reset and not on an new STE
		 * being used.
		 * Although, this is not architectural we rely on the current Linux
		 * implementation.
		 */
		if ((FIELD_GET(CMDQ_CFGI_1_RANGE, command[1]) != 31))
			return true;
		break;
	}
	case CMDQ_OP_TLBI_NH_ASID:
	case CMDQ_OP_TLBI_NH_VA:
	case CMDQ_OP_TLBI_NH_ALL:
	case 0x13: /* CMD_TLBI_NH_VAA: Not used by Linux */
	{
		/* Only allow VMID = 0 */
		if (FIELD_GET(CMDQ_TLBI_0_VMID, command[0]) != 0)
			return true;
		break;
	}
	case CMDQ_OP_PREFETCH_CFG:
	case CMDQ_OP_CFGI_CD:
	case CMDQ_OP_CFGI_CD_ALL:
	case CMDQ_OP_TLBI_NSNH_ALL:
	case CMDQ_OP_PRI_RESP:
	case CMDQ_OP_RESUME:
		break;
	case CMDQ_OP_CMD_SYNC:
		if (FIELD_GET(CMDQ_SYNC_0_CS, command[0]) == CMDQ_SYNC_0_CS_IRQ) {
			/* Do not allow MSI */
			command[0] &= ~CMDQ_SYNC_0_CS;
			command[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
			command[1] &= ~CMDQ_SYNC_1_MSIADDR_MASK;
		}
		break;
	default:
		/* Deny unknown commands */
		return true;
	}

	return false;
}

static int smmu_emulate_cmdq_insert(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 *host_cmdq = hyp_phys_to_virt(smmu->cmdq_host.base_dma);
	bool use_wfe = smmu->features & ARM_SMMU_FEAT_SEV;
	u64 cmd[CMDQ_ENT_DWORDS];
	int idx, ret;
	u32 pending;
	bool skip;

	if (!is_cmdq_enabled(smmu))
		return 0;

	pending = (1 << (smmu->cmdq_host.llq.max_n_shift)) - queue_space(&smmu->cmdq_host.llq);

	hyp_spin_lock(&smmu->hw_lock);
	/* Wait for the command queue to have some space. */
	ret = smmu_wait(use_wfe, smmu_cmdq_has_space(&smmu->cmdq, pending));
	if (ret) {
		hyp_spin_unlock(&smmu->hw_lock);
		return ret;
	}

	while (pending--) {
		int i;

		idx = Q_IDX(&smmu->cmdq_host.llq, smmu->cmdq_host.llq.cons);
		queue_inc_cons(&smmu->cmdq_host.llq);

		/* Copy the command to local buffer avoiding TOCTOU */
		for (i = 0; i < CMDQ_ENT_DWORDS; ++i)
			cmd[i] = le64_to_cpu(READ_ONCE(host_cmdq[idx * CMDQ_ENT_DWORDS + i]));

		skip = smmu_filter_command(smmu, cmd);
		if (WARN_ON(skip))
			continue;
		smmu_add_cmd_raw(smmu, cmd);
	}

	writel(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);

	ret = smmu_wait(use_wfe, smmu_cmdq_empty(&smmu->cmdq));
	hyp_spin_unlock(&smmu->hw_lock);
	return ret;
}

static int smmu_update_ste_shadow(struct hyp_arm_smmu_v3_device *smmu, bool enabled)
{
	size_t size;
	u32 fmt = FIELD_GET(STRTAB_BASE_CFG_FMT, smmu->host_ste_cfg);

	/* Linux doesn't change the fmt nor size of the strtab in the run time. */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		if ((fmt != STRTAB_BASE_CFG_FMT_2LVL) ||
		    (strtab_split(smmu) != STRTAB_SPLIT) ||
		    (strtab_log2size(smmu) > (ilog2(STRTAB_MAX_L1_ENTRIES) + STRTAB_SPLIT)) ||
		    (strtab_split(smmu) >= strtab_log2size(smmu)))
			return -EINVAL;
		size = strtab_l1_size(smmu);
	} else {
		if ((fmt != STRTAB_BASE_CFG_FMT_LINEAR) ||
		    (strtab_log2size(smmu) > smmu->sid_bits))
			return -EINVAL;
		size = strtab_size(smmu);
	}

	if (enabled)
		return smmu_share_pages(strtab_host_base(smmu), size);

	return smmu_unshare_pages(strtab_host_base(smmu), size);
}

static int smmu_flush_all_tlb(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	u32 cr0;
	struct arm_smmu_cmd cmd = arm_smmu_make_cmd_op(CMDQ_OP_TLBI_NSNH_ALL);

	hyp_spin_lock(&smmu->hw_lock);
	/*
	 * This must be called when the SMMU is getting enabled.
	 * First enable the cmdq and then invalidate the TLB.
	 */
	cr0 = readl_relaxed(smmu->base + ARM_SMMU_CR0);
	if (!(cr0 & CR0_CMDQEN)) {
		cr0 |= CR0_CMDQEN;
		writel_relaxed(cr0, smmu->base + ARM_SMMU_CR0);
		ret = smmu_wait(false, readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == cr0);
		if (ret) {
			hyp_spin_unlock(&smmu->hw_lock);
			return ret;
		}
	}

	ret = smmu_send_cmd(smmu, &cmd);
	hyp_spin_unlock(&smmu->hw_lock);
	return ret;
}

static void smmu_emulate_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	/* Enabling SMMU without CMDQ, means TLB invalidation won't work. */
	if (WARN_ON(!is_cmdq_enabled(smmu)))
		return;

	WARN_ON(smmu_update_ste_shadow(smmu, true));
	/* Clean the TLBs each time the SMMU is enabled. */
	WARN_ON(smmu_flush_all_tlb(smmu));
}

static void smmu_emulate_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(smmu_update_ste_shadow(smmu, false));
}

static void smmu_emulate_cmdq_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 shift = smmu->cmdq_host.q_base & Q_BASE_LOG2SIZE;

	smmu->cmdq_host.llq.max_n_shift = min(shift, smmu->cmdq_max_shift);
	smmu->cmdq_host.base_dma = smmu->cmdq_host.q_base & Q_BASE_ADDR_MASK;
	smmu->cmdq_host.base_dma &= ~(cmdq_size(&smmu->cmdq_host) - 1);
	WARN_ON(smmu_share_pages(smmu->cmdq_host.base_dma,
				 cmdq_size(&smmu->cmdq_host)));
}

static void smmu_emulate_cmdq_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	/*
	 * We can not enable the SMMU if the CMDQ is disabled and similarly
	 * we can not disable the CMDQ if the SMMU is enabled, as that can
	 * lead to stale TLBs.
	 */
	WARN_ON(is_smmu_enabled(smmu));

	WARN_ON(smmu_unshare_pages(smmu->cmdq_host.base_dma,
				   cmdq_size(&smmu->cmdq_host)));
}

static void smmu_emulate_queue(struct hyp_arm_smmu_v3_device *smmu,
			       unsigned long q_base, size_t ent_size_shift)
{
	/* Q_BASE_ADDR_MASK is not enough as the SMMU also ignores bits > OAS */
	phys_addr_t base = q_base & Q_BASE_ADDR_MASK & ((1ULL << smmu->oas) - 1);
	size_t size = 1UL << (FIELD_GET(Q_BASE_LOG2SIZE, q_base) + ent_size_shift);

	/* The host must program the base first and it must be aligned to size. */
	if (WARN_ON(!q_base || (base & (size - 1))))
		return;

	WARN_ON(smmu_share_pages(base, size));
}

static bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
			     struct user_pt_regs *regs,
			     u64 esr, u32 off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	const u64 read_write = -1ULL;
	const u64 no_access = 0;
	u64 mask = no_access;
	const u64 read_only = is_write ? no_access : read_write;
	bool is_xzr = (rd == 31);
	u64 val = is_xzr ? 0 : regs->regs[rd];

	switch (off) {
	case ARM_SMMU_IDR0:
		if (len != sizeof(u32))
			break;
		/*
		 * Clear stage-2 support, hide MSI to avoid write back to cmdq,
		 * and hide ATS as it is not emulated or handled in TLB invalidation.
		 */
		mask = read_only & ~(IDR0_S2P | IDR0_VMID16 | IDR0_MSI | IDR0_HYP | IDR0_ATS);
		break;
	case ARM_SMMU_CMDQ_BASE:
		/*
		 * Although allowed to use smaller size, we rely on the SMMUv3 driver
		 * using 64-bit store instruction for simplicity.
		 */
		if (len != sizeof(u64))
			break;
		if (is_write) {
			/* Not allowed by the architecture */
			if (is_cmdq_enabled(smmu))
				break;
			smmu->cmdq_host.q_base = val;
			goto out_ret;
		} else {
			val = smmu->cmdq_host.q_base;
			goto out_update_regs;
		}
	case ARM_SMMU_CMDQ_PROD:
		if (len != sizeof(u32))
			break;
		if (is_write) {
			smmu->cmdq_host.llq.prod = val;
			WARN_ON(smmu_emulate_cmdq_insert(smmu));
			goto out_ret;
		} else {
			val = smmu->cmdq_host.llq.prod;
			goto out_update_regs;
		}
	case ARM_SMMU_CMDQ_CONS:
		if (len != sizeof(u32))
			break;
		if (is_write) {
			if (WARN_ON(is_cmdq_enabled(smmu)))
				break;

			smmu->cmdq_host.llq.cons = val;
			goto out_ret;
		} else {
			/* Propagate errors back to the host.*/
			u32 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);

			val = smmu->cmdq_host.llq.cons | (CMDQ_CONS_ERR & cons);
			goto out_update_regs;
		}
	case ARM_SMMU_STRTAB_BASE:
		if (len != sizeof(u64))
			break;
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			if (is_smmu_enabled(smmu))
				break;
			smmu->host_ste_base = val;
			goto out_ret;
		} else {
			val = smmu->host_ste_base;
			goto out_update_regs;
		}
	case ARM_SMMU_STRTAB_BASE_CFG:
		if (len != sizeof(u32))
			break;
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			if (is_smmu_enabled(smmu))
				break;
			smmu->host_ste_cfg = val;
			goto out_ret;
		} else {
			val = smmu->host_ste_cfg;
			goto out_update_regs;
		}
	case ARM_SMMU_GBPA:
		if (len != sizeof(u32))
			break;

		/* Ignore write, always read to abort. */
		if (!is_write) {
			val = GBPA_ABORT;
			goto out_update_regs;
		}
		goto out_ret;
	case ARM_SMMU_CR0:
		if (len != sizeof(u32))
			break;
		if (is_write) {
			bool last_cmdq_en = is_cmdq_enabled(smmu);
			bool last_smmu_en = is_smmu_enabled(smmu);
			bool last_evtq_en = is_evtq_enabled(smmu);
			bool last_priq_en = is_priq_enabled(smmu);

			smmu->cr0 = val;
			if (!last_cmdq_en && is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_enable(smmu);
			else if (last_cmdq_en && !is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_disable(smmu);

			/*
			 * Share PRI and EVTQ to avoid the host using them to write to
			 * protected memory. However, do not unshare the queues at disable
			 * as that is more complicated, unsharing from here can lead to
			 * use-after-unshare issues, and requires ordering with cr0ack.
			 * The host can disable those queues during shutdown, but it never
			 * changes the base address (even with RPM), so leave the queue
			 * shared and assert that multiple host writes do not change it.
			 */
			if (!last_evtq_en && is_evtq_enabled(smmu) && !smmu->evtq_shared) {
				smmu_emulate_queue(smmu, smmu->evtq_base, EVTQ_ENT_SZ_SHIFT);
				smmu->evtq_shared = true;
			}

			if (!last_priq_en && is_priq_enabled(smmu) && !smmu->priq_shared) {
				smmu_emulate_queue(smmu, smmu->priq_base, PRIQ_ENT_SZ_SHIFT);
				smmu->priq_shared = true;
			}

			if (!last_smmu_en && is_smmu_enabled(smmu))
				smmu_emulate_enable(smmu);
			else if (last_smmu_en && !is_smmu_enabled(smmu))
				smmu_emulate_disable(smmu);
		}
		mask = read_write;
		break;
	case ARM_SMMU_CR1:
		if (len != sizeof(u32))
			break;
		/* Don't mess with shareability/cacheability. */
		if (is_write &&
		    (val != (FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
			     FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
			     FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
			     FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
			     FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
			     FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB))))
			break;

		mask = read_write;
		break;
	case ARM_SMMU_EVTQ_BASE:
		if (len != sizeof(u64))
			break;

		if (is_write) {
			/* See ARM_SMMU_CR0 */
			if (is_evtq_enabled(smmu) ||
			    (smmu->evtq_shared && (smmu->evtq_base != val)))
				break;
			smmu->evtq_base = val;
		}
		mask = read_write;
		break;

	case ARM_SMMU_PRIQ_BASE:
		if (len != sizeof(u64))
			break;

		if (is_write) {
			/* See ARM_SMMU_CR0 */
			if (is_priq_enabled(smmu) ||
			    (smmu->priq_shared && (smmu->priq_base != val)))
				break;
			smmu->priq_base = val;
		}
		mask = read_write;
		break;

	/* Allowed 32 bit registers. */
	case ARM_SMMU_EVTQ_IRQ_CFG1:
	case ARM_SMMU_EVTQ_IRQ_CFG2:
	case ARM_SMMU_GERROR_IRQ_CFG1:
	case ARM_SMMU_GERROR_IRQ_CFG2:
	case ARM_SMMU_PRIQ_IRQ_CFG1:
	case ARM_SMMU_PRIQ_IRQ_CFG2:
		/* These are RES0 as MSI support is hidden. */
		val = 0;
		if (!is_write)
			goto out_update_regs;
		fallthrough;
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
	case ARM_SMMU_PRIQ_PROD + SZ_64K:
	case ARM_SMMU_PRIQ_CONS + SZ_64K:
	case ARM_SMMU_GERRORN:
	case ARM_SMMU_IRQ_CTRLACK:
	case ARM_SMMU_IRQ_CTRL:
	case ARM_SMMU_CR0ACK:
	case ARM_SMMU_CR2:
		if (len != sizeof(u32))
			break;
		mask = read_write;
		break;
	/* Allowed 64 bit registers. */
	case ARM_SMMU_EVTQ_IRQ_CFG0:
	case ARM_SMMU_PRIQ_IRQ_CFG0:
	case ARM_SMMU_GERROR_IRQ_CFG0:
		if (len != sizeof(u64))
			break;
		/* These are RES0 as MSI support is hidden. */
		val = 0;
		if (!is_write)
			goto out_update_regs;
		mask = read_write;
		break;
	/* Allowed RO 32 bit registers. */
	case ARM_SMMU_IDR1:
		if (len != sizeof(u32))
			break;
		/* Cap CMDQS to the shadow queue size. */
		if (!is_write) {
			val = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
			val &= ~IDR1_CMDQS;
			val |= FIELD_PREP(IDR1_CMDQS, smmu->cmdq.llq.max_n_shift);
			goto out_update_regs;
		}
	fallthrough;
	case ARM_SMMU_IIDR:
	case ARM_SMMU_IDR5:
	case ARM_SMMU_IDR3:
	case ARM_SMMU_GERROR:
		if (len != sizeof(u32))
			break;
		mask = read_only;
		break;
	}

	if (WARN_ON(!mask))
		goto out_ret;

	hyp_spin_lock(&smmu->hw_lock);
	if (is_write) {
		if (len == sizeof(u64))
			writeq_relaxed(val & mask, smmu->base + off);
		else
			writel_relaxed(val & mask, smmu->base + off);

		/*
		 * Make sure writes to CR0 are immediately observed, that is important
		 * when synchronizing with TLB invalidation as reading CR0 is not enough
		 * to deduce the SMMU state, and we have to enforce the ack with the
		 * hw_lock acquired.
		 */
		if (off == ARM_SMMU_CR0) {
			u32 cr0 = val;

			WARN_ON(smmu_wait(false,
					  readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == cr0));
			smmu->cmdq_active = !!(cr0 & CR0_CMDQEN);
		}

		hyp_spin_unlock(&smmu->hw_lock);
		return true;
	}

	if (len == sizeof(u64))
		val = readq_relaxed(smmu->base + off) & mask;
	else
		val = readl_relaxed(smmu->base + off) & mask;
	hyp_spin_unlock(&smmu->hw_lock);
out_update_regs:
	/*
	 * Device might be read sensitive, so do it but ignore writing
	 * back for xzr.
	 */
	if (!is_xzr)
		regs->regs[rd] = val;

out_ret:
	return true;
}

static bool smmu_dabt_handler(struct user_pt_regs *regs, u64 esr, u64 addr)
{
	struct hyp_arm_smmu_v3_device *smmu;
	bool ret;

	for_each_smmu(smmu) {
		if (addr < smmu->mmio_addr || addr >= smmu->mmio_addr + smmu->mmio_size)
			continue;
		hyp_spin_lock(&smmu->lock);
		ret = smmu_dabt_device(smmu, regs, esr, addr - smmu->mmio_addr);
		hyp_spin_unlock(&smmu->lock);
		return ret;
	}
	return false;
}

static size_t smmu_pgsize_idmap(size_t size, u64 paddr, size_t pgsize_bitmap)
{
	size_t pgsizes;

	/* Remove page sizes that are larger than the current size */
	pgsizes = pgsize_bitmap & GENMASK_ULL(__fls(size), 0);

	/* Remove page sizes that the address is not aligned to. */
	if (likely(paddr))
		pgsizes &= GENMASK_ULL(__ffs(paddr), 0);

	WARN_ON(!pgsizes);

	/* Return the largest page size that fits. */
	return BIT(__fls(pgsizes));
}

static int smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
	size_t pgsize = PAGE_SIZE, pgcount, size;
	struct io_pgtable *pgtable = idmap_pgtable;
	int ret = 0;

	end = min(end, BIT(pgtable->cfg.oas));
	if (start >= end)
		return 0;

	size = end - start;
	if (prot) {
		size_t mapped;

		if (!(prot & IOMMU_MMIO))
			prot |= IOMMU_CACHE;

		while (size) {
			mapped = 0;
			/*
			 * We handle pages size for memory and MMIO differently:
			 * - memory: Map everything with PAGE_SIZE, that is guaranteed to
			 *   find memory as we allocated enough pages to cover the entire
			 *   memory, we do that as io-pgtable-arm doesn't support
			 *   split_blk_unmap logic any more, so we can't break blocks once
			 *   mapped to tables.
			 * - MMIO: Unlike memory, pKVM allocates 1G for all MMIO, while
			 *   the MMIO space can be large, as it is assumed to cover the
			 *   whole IAS that is not memory, we have to use block mappings,
			 *   that is fine for MMIO as it is never donated at the moment,
			 *   so we never need to unmap MMIO at the run time triggering
			 *   split block logic.
			 */
			if (prot & IOMMU_MMIO)
				pgsize = smmu_pgsize_idmap(size, start, pgtable->cfg.pgsize_bitmap);

			pgcount = size / pgsize;
			ret = pgtable->ops.map_pages(&pgtable->ops, start, start,
						     pgsize, pgcount, prot, 0, &mapped);
			size -= mapped;
			start += mapped;

			if (ret == -EEXIST) {
				/*
				 * It is possible to get EEXIST when a VM dies with pages
				 * in a shared state.
				 */
				ret = 0;
				size -= pgsize;
				start += pgsize;
				continue;
			}
			if (!mapped || ret)
				break;
		}
	} else {
		struct iommu_iotlb_gather gather;
		size_t unmapped;

		while (size) {
			pgcount = size / pgsize;
			iommu_iotlb_gather_init(&gather);
			unmapped = pgtable->ops.unmap_pages(&pgtable->ops, start,
							    pgsize, pgcount, &gather);
			size -= unmapped;
			start += unmapped;
			if (!unmapped)
				break;
		}
	}

	if (ret)
		return ret;

	if (WARN_ON(size))
		return -EINVAL;

	return 0;
}

/* Shared with the kernel driver in EL1 */
struct pkvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
	.dabt_handler			= smmu_dabt_handler,
};
