// SPDX-License-Identifier: GPL-2.0
/*
 * Implementation of the kdump stream table adoption for ARM SMMUv3
 *
 * When the crashed kernel left the SMMU enabled with in-flight DMAs, the kdump
 * kernel adopts the crashed kernel's stream tables, instead of doing a regular
 * reset, to keep in-flight DMAs translating until the endpoint device drivers
 * re-probe and quiesce their devices.
 *
 * Note:
 *  - Adoption only starts on an SMMU that the crashed kernel left enabled, as a
 *    disabled SMMU (CR0_SMMUEN=0) could hold meaningless register values.
 *  - Values read from the crashed kernel's registers get structural validation
 *    only (format, size, span, alignment, and ID range); the physical addresses
 *    are not vetted, as the kdump kernel has no record of which pages held the
 *    tables.
 *  - A structural inconsistency at adoption time tosses the entire adoption and
 *    makes the SMMU fall back to a full reset blocking in-flight DMAs.
 *  - L2 stream tables are adopted lazily at master-inserting time, to bound the
 *    peak memory use against a corrupted L1 table; any lazy L2 adoption failure
 *    rejects that device alone, as its blast radius is bounded to the bus.
 *  - Only a coherent SMMU (ARM_SMMU_FEAT_COHERENCY) is supported, as the stream
 *    table adoption is done by memremap with MEMREMAP_WB, which is verified on
 *    the real hardware. Callers of these functions are responsible for gating
 *    ARM_SMMU_FEAT_COHERENCY once during the probe.
 */

#define dev_fmt(fmt) "kdump: " fmt

#include <linux/io.h>
#include <linux/slab.h>

#include "arm-smmu-v3.h"

/*
 * Commit or roll back an entire scan atomically, so that a concurrently
 * probing SMMU (e.g. forced by driver_async_probe) can only dedup against
 * the reservations that will persist
 */
static DEFINE_MUTEX(arm_smmu_kdump_resv_lock);

/* Identifies the current scan's reservations; serialized by the lock above */
static unsigned long arm_smmu_kdump_scan_id;

/*
 * The adopted stream table keeps translating in-flight DMAs, so the SMMU also
 * keeps caching TLB entries tagged with the crashed kernel's ASIDs and VMIDs.
 * Reserve all the in-use IDs, so that this kernel cannot give its own domains
 * an overlapping ID that would alias the crashed kernel's TLB entries.
 *
 * The reservations are only released when the entire adoption is tossed, as
 * the full-reset fallback flushes the entire TLB. Otherwise, they are kept
 * for the lifetime of the kdump kernel, which reboots after saving a vmcore.
 */
static int arm_smmu_kdump_resv_asid(struct arm_smmu_device *smmu, u32 asid)
{
	int ret;

	/* A valid CD never has ASID 0; both kernels share the same HW limit */
	if (!asid || asid >= 1UL << smmu->asid_bits)
		return -EINVAL;

	guard(mutex)(&arm_smmu_asid_lock);

	/* The value entry marks the ASID as in-use and identifies its scan */
	ret = xa_insert(&arm_smmu_asid_xa, asid,
			xa_mk_value(arm_smmu_kdump_scan_id), GFP_KERNEL);
	/*
	 * An -EBUSY against a value entry is just a safe dedup against another
	 * permanent reservation. A pointer entry means a live domain that will
	 * free its ASID for reuse eventually: keep -EBUSY to toss the adoption.
	 */
	if (ret == -EBUSY && xa_is_value(xa_load(&arm_smmu_asid_xa, asid)))
		ret = 0;
	return ret;
}

static int arm_smmu_kdump_resv_vmid(struct arm_smmu_device *smmu, u32 vmid)
{
	int ret;

	/* A translating STE never has VMID 0, which is reserved for bypass */
	if (!vmid || vmid >= 1UL << smmu->vmid_bits)
		return -EINVAL;

	ret = ida_alloc_range(&smmu->vmid_map, vmid, vmid, GFP_KERNEL);
	if (ret < 0 && ret != -ENOSPC) /* -ENOSPC means already reserved */
		return ret;
	return 0;
}

static int arm_smmu_kdump_resv_cd_asids(struct arm_smmu_device *smmu,
					struct arm_smmu_cd *cds, u32 num_cds)
{
	int ret = 0;
	u32 i;

	for (i = 0; i < num_cds; i++) {
		u64 val = le64_to_cpu(cds[i].data[0]);
		u32 asid = FIELD_GET(CTXDESC_CD_0_ASID, val);

		if (!(val & CTXDESC_CD_0_V))
			continue;
		ret = arm_smmu_kdump_resv_asid(smmu, asid);
		if (ret)
			break;
	}
	return ret;
}

static int arm_smmu_kdump_resv_s1_asids(struct arm_smmu_device *smmu, u64 ste0)
{
	phys_addr_t cdtab = ste0 & STRTAB_STE_0_S1CTXPTR_MASK;
	u32 s1cdmax = FIELD_GET(STRTAB_STE_0_S1CDMAX, ste0);
	u32 s1fmt = FIELD_GET(STRTAB_STE_0_S1FMT, ste0);
	size_t max_contexts = 1UL << s1cdmax;
	struct arm_smmu_cdtab_l1 *l1tab;
	u32 num_l1_ents, num_cds, i;
	int ret = 0;

	if (!cdtab || s1cdmax > smmu->ssid_bits)
		return -EINVAL;

	if (s1fmt == STRTAB_STE_0_S1FMT_LINEAR) {
		struct arm_smmu_cd *cds;

		/*
		 * A crashed kernel might have used a linear CD table on the
		 * 2-level capable hardware like the linear stream table case.
		 * So, accept it here as well. s1cdmax is capped by ssid_bits.
		 */
		cds = memremap(cdtab, max_contexts * sizeof(*cds), MEMREMAP_WB);
		if (!cds)
			return -ENOMEM;
		ret = arm_smmu_kdump_resv_cd_asids(smmu, cds, max_contexts);
		memunmap(cds);
		return ret;
	}

	if (s1fmt != STRTAB_STE_0_S1FMT_64K_L2)
		return -EINVAL;

	num_l1_ents = DIV_ROUND_UP(max_contexts, CTXDESC_L2_ENTRIES);
	l1tab = memremap(cdtab, num_l1_ents * sizeof(*l1tab), MEMREMAP_WB);
	if (!l1tab)
		return -ENOMEM;

	/* max_contexts being under a full leaf makes the only leaf partial */
	num_cds = min_t(size_t, max_contexts, CTXDESC_L2_ENTRIES);

	/* Aliased L2 tables cannot extend the walk; they only repeat a scan */
	for (i = 0; i < num_l1_ents; i++) {
		u64 l1_desc = le64_to_cpu(l1tab[i].l2ptr);
		struct arm_smmu_cdtab_l2 *l2;

		if (!(l1_desc & CTXDESC_L1_DESC_V))
			continue;

		l2 = memremap(l1_desc & CTXDESC_L1_DESC_L2PTR_MASK,
			      num_cds * sizeof(*l2->cds), MEMREMAP_WB);
		if (!l2) {
			ret = -ENOMEM;
			break;
		}
		ret = arm_smmu_kdump_resv_cd_asids(smmu, l2->cds, num_cds);
		memunmap(l2);
		if (ret)
			break;
	}
	memunmap(l1tab);
	return ret;
}

static int arm_smmu_kdump_resv_ste_ids(struct arm_smmu_device *smmu,
				       struct arm_smmu_ste *ste)
{
	u32 vmid = FIELD_GET(STRTAB_STE_2_S2VMID, le64_to_cpu(ste->data[2]));
	u64 val = le64_to_cpu(ste->data[0]);

	if (!(val & STRTAB_STE_0_V))
		return 0;

	switch (FIELD_GET(STRTAB_STE_0_CFG, val)) {
	case STRTAB_STE_0_CFG_ABORT:
	case STRTAB_STE_0_CFG_BYPASS:
		return 0;
	case STRTAB_STE_0_CFG_S1_TRANS:
		return arm_smmu_kdump_resv_s1_asids(smmu, val);
	case STRTAB_STE_0_CFG_NESTED:
		/*
		 * A guest-owned CD table is in IPA space, unreachable. Its
		 * ASIDs are only tagged with the S2VMID reserved below, so
		 * they cannot alias this kernel's VMID-0 or EL2 S1 domains.
		 */
		fallthrough;
	case STRTAB_STE_0_CFG_S2_TRANS:
		return arm_smmu_kdump_resv_vmid(smmu, vmid);
	default:
		return -EINVAL;
	}
}

static int arm_smmu_kdump_resv_ids(struct arm_smmu_device *smmu)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	int ret = 0;
	u32 i, j;

	lockdep_assert_held(&arm_smmu_kdump_resv_lock);

	/* Allocate a new ID for this scan, to scope its rollback */
	arm_smmu_kdump_scan_id++;

	if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)) {
		for (i = 0; i < cfg->linear.num_ents; i++) {
			ret = arm_smmu_kdump_resv_ste_ids(
				smmu, &cfg->linear.table[i]);
			if (ret)
				return ret;
		}
		return 0;
	}

	/* Aliased L2 tables cannot extend the walk; they only repeat a scan */
	for (i = 0; i < cfg->l2.num_l1_ents; i++) {
		u64 l1_desc = le64_to_cpu(cfg->l2.l1tab[i].l2ptr);
		phys_addr_t base = l1_desc & STRTAB_L1_DESC_L2PTR_MASK;
		u32 span = FIELD_GET(STRTAB_L1_DESC_SPAN, l1_desc);
		struct arm_smmu_strtab_l2 *l2;

		if (!span)
			continue;

		/* Validated at adopt time, so a change means live corruption */
		if (span != STRTAB_SPLIT + 1 || !base)
			return -EINVAL;

		/* Transient map: L2 tables are only adopted upon device use */
		l2 = memremap(base, sizeof(*l2), MEMREMAP_WB);
		if (!l2)
			return -ENOMEM;
		for (j = 0; j < ARRAY_SIZE(l2->stes); j++) {
			ret = arm_smmu_kdump_resv_ste_ids(smmu, &l2->stes[j]);
			if (ret)
				break;
		}
		memunmap(l2);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Roll back a scan that has not committed yet. Committed reservations are
 * never released, as another SMMU's scan might have deduped against them.
 */
static void arm_smmu_kdump_unresv_ids(struct arm_smmu_device *smmu)
{
	unsigned long index;
	void *entry;

	lockdep_assert_held(&arm_smmu_kdump_resv_lock);

	mutex_lock(&arm_smmu_asid_lock);
	xa_for_each(&arm_smmu_asid_xa, index, entry) {
		if (entry == xa_mk_value(arm_smmu_kdump_scan_id))
			xa_erase(&arm_smmu_asid_xa, index);
	}
	mutex_unlock(&arm_smmu_asid_lock);

	/* No domain exists yet, so the ida holds only the reservations */
	ida_destroy(&smmu->vmid_map);
}

int arm_smmu_kdump_adopt_deferred_l2_strtab(struct arm_smmu_device *smmu,
					    u32 sid, phys_addr_t base, u32 span,
					    struct arm_smmu_strtab_l2 **l2table)
{
	struct arm_smmu_strtab_l2 *table;
	size_t size;

	/*
	 * Retest the span in case the L1 descriptor has been overwritten since
	 * the adopt. Reject this master's insert; panic or SMMU-disable would
	 * either lose the vmcore or cascade aborts. Do not try to fix it, as it
	 * would break all other SIDs in the same bus (PCI case). The corruption
	 * blast radius is already bounded to that bus range.
	 */
	if (span != STRTAB_SPLIT + 1) {
		dev_err(smmu->dev,
			"L1[%u] span %u changed since adopt (was %u)\n",
			arm_smmu_strtab_l1_idx(sid), span, STRTAB_SPLIT + 1);
		return -EINVAL;
	}

	size = (1UL << (span - 1)) * sizeof(struct arm_smmu_ste);

	/* Same live-corruption check as the span; reject an overwritten base */
	if (!base || !IS_ALIGNED(base, size)) {
		dev_err(smmu->dev, "L1[%u] bad l2 table base %pa\n",
			arm_smmu_strtab_l1_idx(sid), &base);
		return -EINVAL;
	}

	/*
	 * This L2 table is mapped lazily per master; devres frees it at unbind,
	 * as with the dmam_alloc_coherent() used for a fresh L2.
	 */
	table = devm_memremap(smmu->dev, base, size, MEMREMAP_WB);
	if (IS_ERR(table)) {
		dev_err(smmu->dev,
			"failed to adopt l2 stream table for SID %u\n", sid);
		return PTR_ERR(table);
	}

	*l2table = table;
	return 0;
}

static int arm_smmu_kdump_adopt_strtab_2lvl(struct arm_smmu_device *smmu,
					    u32 cfg_reg, phys_addr_t base)
{
	u32 log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, cfg_reg);
	u32 split = FIELD_GET(STRTAB_BASE_CFG_SPLIT, cfg_reg);
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	u32 num_l1_ents;
	size_t size;
	int i;

	if (log2size < split || log2size > smmu->sid_bits) {
		dev_err(smmu->dev, "log2size %u out of range [%u, %u]\n",
			log2size, split, smmu->sid_bits);
		return -EINVAL;
	}
	if (split != STRTAB_SPLIT) {
		dev_err(smmu->dev,
			"unsupported STRTAB_SPLIT %u (expected %u)\n", split,
			STRTAB_SPLIT);
		return -EINVAL;
	}

	num_l1_ents = 1U << (log2size - split);
	if (num_l1_ents > STRTAB_MAX_L1_ENTRIES) {
		dev_err(smmu->dev, "l1 entries %u exceeds max %u\n",
			num_l1_ents, STRTAB_MAX_L1_ENTRIES);
		return -EINVAL;
	}

	cfg->l2.num_l1_ents = num_l1_ents;

	size = num_l1_ents * sizeof(struct arm_smmu_strtab_l1);
	if (!IS_ALIGNED(base, size)) {
		dev_err(smmu->dev, "unaligned l1 stream table base %pa\n",
			&base);
		return -EINVAL;
	}

	cfg->l2.l1tab = memremap(base, size, MEMREMAP_WB);
	if (!cfg->l2.l1tab)
		return -ENOMEM;

	cfg->l2.l2ptrs =
		kcalloc(num_l1_ents, sizeof(*cfg->l2.l2ptrs), GFP_KERNEL);
	if (!cfg->l2.l2ptrs)
		return -ENOMEM;

	for (i = 0; i < num_l1_ents; i++) {
		u64 l2ptr = le64_to_cpu(cfg->l2.l1tab[i].l2ptr);
		phys_addr_t l2_base = l2ptr & STRTAB_L1_DESC_L2PTR_MASK;
		u32 span = FIELD_GET(STRTAB_L1_DESC_SPAN, l2ptr);

		if (!span)
			continue;

		if (span != STRTAB_SPLIT + 1) {
			dev_err(smmu->dev,
				"L1[%u] unsupported span %u (vs %u)\n", i, span,
				STRTAB_SPLIT + 1);
			return -EINVAL;
		}

		if (!l2_base ||
		    !IS_ALIGNED(l2_base, sizeof(struct arm_smmu_strtab_l2))) {
			dev_err(smmu->dev, "L1[%u] bad l2 table base %pa\n", i,
				&l2_base);
			return -EINVAL;
		}

		/*
		 * If the crashed kernel's l1 descriptors are deeply corrupted,
		 * blindly memremapping every l2 table here could lead to OOM.
		 *
		 * Defer the l2 memremap to arm_smmu_init_l2_strtab(), so peak
		 * memory is bounded by the kdump kernel's actual demand.
		 */
	}

	return 0;
}

static int arm_smmu_kdump_adopt_strtab_linear(struct arm_smmu_device *smmu,
					      u32 cfg_reg, phys_addr_t base)
{
	u32 log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, cfg_reg);
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	unsigned int max_log2size;
	size_t size;

	/* Cap the size at what the kdump kernel itself would have allocated */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
		max_log2size =
			ilog2(STRTAB_MAX_L1_ENTRIES * STRTAB_NUM_L2_STES);
	else
		max_log2size = smmu->sid_bits;

	/* cfg->linear.num_ents is unsigned int, so cap log2size at 31 */
	max_log2size = min(max_log2size, 31U);
	if (log2size > max_log2size) {
		dev_err(smmu->dev, "unsupported log2size %u (> %u)\n", log2size,
			max_log2size);
		return -EINVAL;
	}

	/*
	 * We might end up with a num_ents != sid_bits, which is fine. In the
	 * ARM_SMMU_OPT_KDUMP_ADOPT case, arm_smmu_write_strtab() is bypassed.
	 */
	cfg->linear.num_ents = 1U << log2size;

	size = cfg->linear.num_ents * sizeof(struct arm_smmu_ste);
	if (!IS_ALIGNED(base, size)) {
		dev_err(smmu->dev, "unaligned stream table base %pa\n", &base);
		return -EINVAL;
	}

	cfg->linear.table = memremap(base, size, MEMREMAP_WB);
	if (!cfg->linear.table)
		return -ENOMEM;
	return 0;
}

static void arm_smmu_kdump_adopt_cleanup(void *data)
{
	struct arm_smmu_device *smmu = data;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;

	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB) {
		kfree(cfg->l2.l2ptrs);
		if (cfg->l2.l1tab)
			memunmap(cfg->l2.l1tab);
	} else {
		if (cfg->linear.table)
			memunmap(cfg->linear.table);
	}
}

int arm_smmu_kdump_adopt_strtab(struct arm_smmu_device *smmu)
{
	u32 cfg_reg = readl_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE_CFG);
	u64 base_reg = readq_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE);
	bool was_2lvl = smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB;
	phys_addr_t base = base_reg & STRTAB_BASE_ADDR_MASK;
	u32 fmt = FIELD_GET(STRTAB_BASE_CFG_FMT, cfg_reg);
	int ret;

	dev_dbg(smmu->dev, "adopting crashed kernel's stream table\n");

	if (fmt == STRTAB_BASE_CFG_FMT_2LVL) {
		/*
		 * Both kernels run on the same hardware, so it's impossible for
		 * kdump kernel to see the support for linear stream table only.
		 */
		if (WARN_ON(!(smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)))
			ret = -EINVAL;
		else
			ret = arm_smmu_kdump_adopt_strtab_2lvl(smmu, cfg_reg,
							       base);
	} else if (fmt == STRTAB_BASE_CFG_FMT_LINEAR) {
		/*
		 * The kdump kernel need not match the crashed kernel. An older
		 * crashed kernel that predates two-level stream table support
		 * may have used a linear table on 2-level-capable hardware, so
		 * enforce the same format here to match the adopted table.
		 */
		ret = arm_smmu_kdump_adopt_strtab_linear(smmu, cfg_reg, base);
		if (!ret)
			smmu->features &= ~ARM_SMMU_FEAT_2_LVL_STRTAB;
	} else {
		dev_err(smmu->dev, "invalid STRTAB format %u\n", fmt);
		ret = -EINVAL;
	}

	if (ret) {
		arm_smmu_kdump_adopt_cleanup(smmu);
		goto err;
	}

	mutex_lock(&arm_smmu_kdump_resv_lock);
	ret = arm_smmu_kdump_resv_ids(smmu);
	if (ret) {
		dev_warn(smmu->dev, "failed to reserve in-use ASIDs/VMIDs\n");
		arm_smmu_kdump_adopt_cleanup(smmu);
		goto err_unresv;
	}

	ret = devm_add_action_or_reset(smmu->dev, arm_smmu_kdump_adopt_cleanup,
				       smmu);
	/* devm_add_action_or_reset ran the cleanup upon failure */
	if (ret) {
		dev_warn(smmu->dev, "failed to set up cleanup action\n");
		goto err_unresv;
	}
	mutex_unlock(&arm_smmu_kdump_resv_lock);

	return 0;

err_unresv:
	/* The full reset will flush the entire TLB, so release everything */
	arm_smmu_kdump_unresv_ids(smmu);
	mutex_unlock(&arm_smmu_kdump_resv_lock);
err:
	dev_warn(smmu->dev, "falling back to full reset\n");
	/*
	 * Undo the linear adoption's clearing of FEAT_2_LVL_STRTAB so that the
	 * full-reset fallback uses the hardware-supported format.
	 */
	if (was_2lvl)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_STRTAB;
	memset(&smmu->strtab_cfg, 0, sizeof(smmu->strtab_cfg));
	smmu->options &= ~ARM_SMMU_OPT_KDUMP_ADOPT;
	return ret;
}

bool arm_smmu_kdump_is_attach_deferred(struct arm_smmu_master *master)
{
	struct arm_smmu_device *smmu = master->smmu;
	int i;

	for (i = 0; i < master->num_streams; i++) {
		struct arm_smmu_ste *ste =
			arm_smmu_get_step_for_sid(smmu, master->streams[i].id);
		u64 ent0 = le64_to_cpu(ste->data[0]);

		/* Defer only when there might be in-flight DMAs */
		if ((ent0 & STRTAB_STE_0_V) &&
		    FIELD_GET(STRTAB_STE_0_CFG, ent0) != STRTAB_STE_0_CFG_ABORT)
			return true;
	}

	return false;
}

void arm_smmu_device_kdump_probe(struct arm_smmu_device *smmu)
{
	u32 gerror, gerrorn, active;

	/* No adoption if SMMU is disabled (i.e., there is no in-flight DMA) */
	if (!(readl_relaxed(smmu->base + ARM_SMMU_CR0) & CR0_SMMUEN))
		return;

	/* For now, only support a coherent SMMU that works with MEMREMAP_WB */
	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY)) {
		dev_warn(smmu->dev,
			 "non-coherent SMMU unsupported; reset to block all DMAs\n");
		return;
	}

	gerror = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	gerrorn = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);
	active = gerror ^ gerrorn;
	if (active & GERROR_SFM_ERR) {
		dev_warn(smmu->dev,
			 "SMMU in Service Failure Mode, must reset\n");
		return;
	}

	smmu->options |= ARM_SMMU_OPT_KDUMP_ADOPT;
}
