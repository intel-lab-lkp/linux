// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <asm/kvm_hyp.h>

#include <nvhe/clock.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/trap_handler.h>

#include "arm_smmu_v3.h"

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

/* strtab accessors */
#define strtab_log2size(smmu)	(FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, (smmu)->host_ste_cfg))
#define strtab_size(smmu)	((1UL << strtab_log2size(smmu)) * STRTAB_STE_DWORDS * 8)
#define strtab_host_base(smmu)	((smmu)->host_ste_base & STRTAB_BASE_ADDR_MASK)
#define strtab_split(smmu)	(FIELD_GET(STRTAB_BASE_CFG_SPLIT, (smmu)->host_ste_cfg))
#define strtab_l1_size(smmu)	((1UL << (strtab_log2size(smmu) - strtab_split(smmu))) * \
				 (sizeof(struct arm_smmu_strtab_l1)))

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

#define cmdq_size(cmdq)	((1 << ((cmdq)->llq.max_n_shift)) * CMDQ_ENT_DWORDS * 8)

/*
 * Wait until @cond is true.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(use_wfe, _cond)					\
({									\
	int __ret = 0;							\
	u64 delay = hyp_clock_ns() + ARM_SMMU_POLL_TIMEOUT_US * 1000;	\
									\
	while (!(_cond)) {						\
		if (use_wfe) {						\
			wfe();						\
			if ((_cond))					\
				break;					\
		} else {						\
			cpu_relax();					\
		}							\
		if (hyp_clock_ns() >= delay) {				\
			__ret = -ETIMEDOUT;				\
			break;						\
		}							\
	}								\
	__ret;								\
})

static bool is_cmdq_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_CMDQEN, smmu->cr0);
}

static bool is_smmu_enabled(struct hyp_arm_smmu_v3_device *smmu)
{
	return FIELD_GET(CR0_SMMUEN, smmu->cr0);
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

	for (i = 0 ; i < nr_pages ; ++i) {
		if (__pkvm_host_share_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT)) {
			while (i--)
				__pkvm_host_unshare_hyp((base + i * PAGE_SIZE) >> PAGE_SHIFT);
			return -EPERM;
		}
	}

	ret = hyp_pin_shared_mem(hyp_phys_to_virt(base),
				 hyp_phys_to_virt(base + nr_pages * PAGE_SIZE));
	if (ret) {
		for (i = 0 ; i < nr_pages ; ++i)
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

	for (i = 0 ; i < nr_pages ; ++i) {
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
			struct arm_smmu_cmdq_ent *ent)
{
	int ret;
	u64 cmd[CMDQ_ENT_DWORDS];

	ret = smmu_wait(false, !smmu_cmdq_full(&smmu->cmdq));
	if (ret)
		return ret;

	ret = arm_smmu_cmdq_build_cmd(cmd, ent);
	if (ret)
		return ret;

	smmu_add_cmd_raw(smmu, cmd);
	writel(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait(smmu->features & ARM_SMMU_FEAT_SEV,
			 smmu_cmdq_empty(&smmu->cmdq));
}

__maybe_unused
static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(smmu, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
}

/* Put the device in a state that can be probed by the host driver. */
static void smmu_deinit_device(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(__pkvm_hyp_donate_host_mmio(smmu->mmio_addr, smmu->mmio_size));

	if (smmu->cmdq.base)
		WARN_ON(__pkvm_hyp_donate_host(smmu->cmdq.base_dma >> PAGE_SHIFT,
					       cmdq_size(&smmu->cmdq) >> PAGE_SHIFT));

	if (smmu->strtab_cfg.linear.table ||
	    smmu->strtab_cfg.l2.l1tab)
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(smmu->strtab_dma),
					       smmu->strtab_size >> PAGE_SHIFT));
	smmu->base = NULL;
}

static bool smmu_nesting_supported(struct hyp_arm_smmu_v3_device *smmu)
{
	unsigned int implementer, productid, variant, revision;
	u32 reg;

	if (!(smmu->features & ARM_SMMU_FEAT_TRANS_S1) ||
	    !(smmu->features & ARM_SMMU_FEAT_TRANS_S2))
		return false;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IIDR);
	implementer = FIELD_GET(IIDR_IMPLEMENTER, reg);
	productid = FIELD_GET(IIDR_PRODUCTID, reg);
	variant = FIELD_GET(IIDR_VARIANT, reg);
	revision = FIELD_GET(IIDR_REVISION, reg);

	if (implementer != IIDR_IMPLEMENTER_ARM)
		return true;

	if (productid == IIDR_PRODUCTID_ARM_MMU_600)
		return variant >= 2;
	else if (productid == IIDR_PRODUCTID_ARM_MMU_700)
		return !(variant < 1 || revision < 1);

	return true;
}

/*
 * Mini-probe and validation for the hypervisor.
 */
static int smmu_probe(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 reg;

	/* Similar to the kernel, rely on firmware override. */
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		return -EINVAL;

	/* IDR0 */
	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR0);

	smmu->features |= smmu_idr0_features(reg);
	if (!smmu_nesting_supported(smmu))
		return -ENXIO;

	if (!(smmu->features & (ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE)))
		return -ENXIO;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR1);
	if (reg & (IDR1_TABLES_PRESET | IDR1_QUEUES_PRESET | IDR1_REL))
		return -EINVAL;

	smmu->sid_bits = FIELD_GET(IDR1_SIDSIZE, reg);
	/* Follows the kernel logic */
	if (smmu->sid_bits <= STRTAB_SPLIT)
		smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR3);
	smmu->features |= smmu_idr3_features(reg);

	reg = readl_relaxed(smmu->base + ARM_SMMU_IDR5);
	smmu->pgsize_bitmap = smmu_idr5_to_pgsize(reg);

	smmu->oas = smmu_idr5_to_oas(reg);
	if (smmu->oas == 52)
		smmu->pgsize_bitmap |= 1ULL << 42;
	else if (!smmu->oas)
		smmu->oas = 48;

	return 0;
}

/*
 * The kernel part of the driver will allocate the shadow cmdq,
 * and zero it. This function only donates it.
 */
static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	size_t cmdq_nr_pages = cmdq_size(&smmu->cmdq) >> PAGE_SHIFT;
	int ret;

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
	ret = __pkvm_host_donate_hyp_mmio(smmu->mmio_addr, smmu->mmio_size, &haddr);
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

	return 0;

out_ret:
	smmu_deinit_device(smmu);
	return ret;
}

/* Called while is the host is still trusted. */
static int smmu_init(void)
{
	size_t smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) *
					  kvm_hyp_arm_smmu_v3_count);
	struct hyp_arm_smmu_v3_device *smmu;
	u64 pfn, nr_pages;
	int ret;

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

	return 0;

out_reclaim_smmu:
	while (smmu != kvm_hyp_arm_smmu_v3_smmus)
		smmu_deinit_device(--smmu);
	WARN_ON(__pkvm_hyp_donate_host(pfn, nr_pages));
	return ret;
}

static bool smmu_filter_command(struct hyp_arm_smmu_v3_device *smmu, u64 *command)
{
	u64 command0 = le64_to_cpu(command[0]);
	u64 command1 = le64_to_cpu(command[1]);
	u64 type = FIELD_GET(CMDQ_0_OP, command0);

	switch (type) {
	case CMDQ_OP_CFGI_STE:
		/* TBD: SHADOW_STE*/
		break;
	case CMDQ_OP_CFGI_ALL:
	{
		/*
		 * Linux doesn't use range STE invalidation, and only use this
		 * for CFGI_ALL, which is done on reset and not on an new STE
		 * being used.
		 * Although, this is not architectural we rely on the current Linux
		 * implementation.
		 */
		if ((FIELD_GET(CMDQ_CFGI_1_RANGE, command1) != 31))
			return true;
		break;
	}
	case CMDQ_OP_TLBI_NH_ASID:
	case CMDQ_OP_TLBI_NH_VA:
	case 0x13: /* CMD_TLBI_NH_VAA: Not used by Linux */
	{
		/* Only allow VMID = 0 */
		if (FIELD_GET(CMDQ_TLBI_0_VMID, command0) != 0)
			return true;
		break;
	}
	case 0x10: /* CMD_TLBI_NH_ALL: Not used by Linux */
	case CMDQ_OP_TLBI_EL2_ALL:
	case CMDQ_OP_TLBI_EL2_VA:
	case CMDQ_OP_TLBI_EL2_ASID:
	case CMDQ_OP_TLBI_S12_VMALL:
	case CMDQ_OP_TLBI_S2_IPA:
	case 0x23: /* CMD_TLBI_EL2_VAA: Not used by Linux */
		return true;
	case CMDQ_OP_CMD_SYNC:
		if (FIELD_GET(CMDQ_SYNC_0_CS, command0) == CMDQ_SYNC_0_CS_IRQ) {
			/* Allow it, but let the host timeout, as this should never happen. */
			command0 &= ~CMDQ_SYNC_0_CS;
			command0 |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
			command1 &= ~CMDQ_SYNC_1_MSIADDR_MASK;
		}
		break;
	}

	return false;
}

static int smmu_emulate_cmdq_insert(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 *host_cmdq = hyp_phys_to_virt(smmu->cmdq_host.q_base & Q_BASE_ADDR_MASK);
	bool use_wfe = smmu->features & ARM_SMMU_FEAT_SEV, skip;
	u64 cmd[CMDQ_ENT_DWORDS];
	int idx, ret;
	u32 space;

	if (!is_cmdq_enabled(smmu))
		return 0;

	space = (1 << (smmu->cmdq_host.llq.max_n_shift)) - queue_space(&smmu->cmdq_host.llq);
	/* Wait for the command queue to have some space. */
	ret = smmu_wait(use_wfe, smmu_cmdq_has_space(&smmu->cmdq, space));
	if (ret)
		return ret;

	while (space--) {
		idx = Q_IDX(&smmu->cmdq_host.llq, smmu->cmdq_host.llq.cons);
		queue_inc_cons(&smmu->cmdq_host.llq);

		memcpy(cmd, &host_cmdq[idx * CMDQ_ENT_DWORDS], CMDQ_ENT_DWORDS << 3);
		skip = smmu_filter_command(smmu, cmd);
		if (WARN_ON(skip))
			continue;
		smmu_add_cmd_raw(smmu, cmd);
	}

	writel(smmu->cmdq.llq.prod, smmu->cmdq.prod_reg);

	return smmu_wait(use_wfe, smmu_cmdq_empty(&smmu->cmdq));
}

static int smmu_update_ste_shadow(struct hyp_arm_smmu_v3_device *smmu, bool enabled)
{
	size_t strtab_size;
	u32 fmt  = FIELD_GET(STRTAB_BASE_CFG_FMT, smmu->host_ste_cfg);

	/* Linux doesn't change the fmt nor size of the strtab in the run time. */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		if ((fmt != STRTAB_BASE_CFG_FMT_2LVL) ||
		     (strtab_split(smmu) != STRTAB_SPLIT) ||
		     (strtab_log2size(smmu) > (ilog2(STRTAB_MAX_L1_ENTRIES) + STRTAB_SPLIT)) ||
		     (strtab_split(smmu) >= strtab_log2size(smmu)))
			return -EINVAL;
		strtab_size = strtab_l1_size(smmu);
	} else {
		if ((fmt != STRTAB_BASE_CFG_FMT_LINEAR) ||
		    (strtab_log2size(smmu) > smmu->sid_bits))
			return -EINVAL;
		strtab_size = strtab_size(smmu);
	}

	if (enabled)
		return smmu_share_pages(strtab_host_base(smmu), strtab_size);

	return smmu_unshare_pages(strtab_host_base(smmu), strtab_size);
}

static void smmu_emulate_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	/* Enabling SMMU without CMDQ, means TLB invalidation won't work. */
	if (WARN_ON(!is_cmdq_enabled(smmu)))
		return;

	WARN_ON(smmu_update_ste_shadow(smmu, true));
}

static void smmu_emulate_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(smmu_update_ste_shadow(smmu, false));
}

static void smmu_emulate_cmdq_enable(struct hyp_arm_smmu_v3_device *smmu)
{
	u32 shift = smmu->cmdq_host.q_base & Q_BASE_LOG2SIZE;

	smmu->cmdq_host.llq.max_n_shift = min(shift, 19);
	smmu->cmdq_host.base_dma = smmu->cmdq_host.q_base & Q_BASE_ADDR_MASK;
	WARN_ON(smmu_share_pages(smmu->cmdq_host.base_dma,
				 cmdq_size(&smmu->cmdq_host)));
}

static void smmu_emulate_cmdq_disable(struct hyp_arm_smmu_v3_device *smmu)
{
	WARN_ON(smmu_unshare_pages(smmu->cmdq_host.base_dma,
				   cmdq_size(&smmu->cmdq_host)));
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
		/* Clear stage-2 support, hide MSI to avoid write back to cmdq */
		mask = read_only & ~(IDR0_S2P | IDR0_VMID16 | IDR0_MSI | IDR0_HYP);
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
			if (WARN_ON(is_cmdq_enabled(smmu)))
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
	/* Passthrough the register access for bisectiblity, handled later */
	case ARM_SMMU_STRTAB_BASE:
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			if (is_smmu_enabled(smmu))
				break;
			smmu->host_ste_base = val;
		}
		mask = read_write;
		break;
	case ARM_SMMU_STRTAB_BASE_CFG:
		if (is_write) {
			/* Must only be written when SMMU_CR0.SMMUEN == 0.*/
			if (is_smmu_enabled(smmu))
				break;
			smmu->host_ste_cfg = val;
		}
		mask = read_write;
		break;
	case ARM_SMMU_GBPA:
		mask = read_write;
		break;
	case ARM_SMMU_CR0:
		if (len != sizeof(u32))
			break;
		if (is_write) {
			bool last_cmdq_en = is_cmdq_enabled(smmu);
			bool last_smmu_en = is_smmu_enabled(smmu);

			smmu->cr0 = val;
			if (!last_cmdq_en && is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_enable(smmu);
			else if (last_cmdq_en && !is_cmdq_enabled(smmu))
				smmu_emulate_cmdq_disable(smmu);
			if (!last_smmu_en && is_smmu_enabled(smmu))
				smmu_emulate_enable(smmu);
			else if (last_smmu_en && !is_smmu_enabled(smmu))
				smmu_emulate_disable(smmu);
		}
		mask = read_write;
		break;
	case ARM_SMMU_CR1: {
		/* Based on Linux implementation */
		u64 cr1_template = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
				FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
				FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
				FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
		if (len != sizeof(u32))
			break;
		/* Don't mess with shareability/cacheability. */
		if (is_write) {
			WARN_ON(val != cr1_template);
			val = cr1_template;
		}
		mask = read_write;
		break;
	}

	/* Allowed 32 bit registers. */
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
	case ARM_SMMU_EVTQ_IRQ_CFG1:
	case ARM_SMMU_EVTQ_IRQ_CFG2:
	case ARM_SMMU_PRIQ_PROD + SZ_64K:
	case ARM_SMMU_PRIQ_CONS + SZ_64K:
	case ARM_SMMU_PRIQ_IRQ_CFG1:
	case ARM_SMMU_PRIQ_IRQ_CFG2:
	case ARM_SMMU_GERRORN:
	case ARM_SMMU_GERROR_IRQ_CFG1:
	case ARM_SMMU_GERROR_IRQ_CFG2:
	case ARM_SMMU_IRQ_CTRLACK:
	case ARM_SMMU_IRQ_CTRL:
	case ARM_SMMU_CR0ACK:
	case ARM_SMMU_CR2:
		if (len != sizeof(u32))
			break;
		mask = read_write;
		break;
	/* Allowed 64 bit registers. */
	case ARM_SMMU_EVTQ_BASE:
	case ARM_SMMU_EVTQ_IRQ_CFG0:
	case ARM_SMMU_PRIQ_BASE:
	case ARM_SMMU_PRIQ_IRQ_CFG0:
	case ARM_SMMU_GERROR_IRQ_CFG0:
		if (len != sizeof(u64))
			break;
		mask = read_write;
		break;
	/* Allowed RO 32 bit registers. */
	case ARM_SMMU_IIDR:
	case ARM_SMMU_IDR5:
	case ARM_SMMU_IDR3:
	case ARM_SMMU_IDR1:
	case ARM_SMMU_GERROR:
		if (len != sizeof(u32))
			break;
		mask = read_only;
	};

	if (WARN_ON(!mask))
		goto out_ret;

	if (is_write) {
		if (len == sizeof(u64))
			writeq_relaxed(val & mask, smmu->base + off);
		else
			writel_relaxed(val & mask, smmu->base + off);

		return true;
	}

	if (len == sizeof(u64))
		val = readq_relaxed(smmu->base + off) & mask;
	else
		val = readl_relaxed(smmu->base + off) & mask;

out_update_regs:
	/*
	 * Device might be read senstive, so do it but ignore writing
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

static int smmu_host_stage2_idmap(phys_addr_t start, phys_addr_t end, int prot)
{
	return 0;
}

/* Shared with the kernel driver in EL1 */
struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.host_stage2_idmap		= smmu_host_stage2_idmap,
	.dabt_handler			= smmu_dabt_handler,
};
