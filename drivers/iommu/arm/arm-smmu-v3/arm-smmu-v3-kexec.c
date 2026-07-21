// SPDX-License-Identifier: GPL-2.0
/*
 * Common helpers for a kexec'd kernel to parse, validate, and walk through the
 * previous kernel's SMMU table structures, shared by the kdump adoption and a
 * future live-update restoration.
 *
 * All of the helpers are read-only against the previous kernel's structures: a
 * table that is not yet mapped by this kernel gets a transient memremap during
 * a walk, followed by an immediate memunmap. They never allocate memory or take
 * ownership of the previous kernel's tables; the callers make those decisions.
 */

#include <linux/io.h>

#include "arm-smmu-v3.h"

/**
 * arm_smmu_kexec_parse_strtab_2lvl() - Validate a 2-level stream table
 * @smmu: SMMU device of this kernel
 * @cfg_reg: STRTAB_BASE_CFG register value set by the previous kernel
 * @base: stream table base address extracted from the STRTAB_BASE register
 * @num_l1_ents: pointer to return the number of L1 entries
 *
 * Validate the 2-level stream table geometry in @cfg_reg and @base's alignment
 * against this kernel's hardware limits.
 *
 * Return: 0 on success with @num_l1_ents set, or -EINVAL on a bad geometry
 */
int arm_smmu_kexec_parse_strtab_2lvl(struct arm_smmu_device *smmu, u32 cfg_reg,
				     phys_addr_t base, u32 *num_l1_ents)
{
	u32 log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, cfg_reg);
	u32 split = FIELD_GET(STRTAB_BASE_CFG_SPLIT, cfg_reg);
	u32 num_ents;
	size_t size;

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

	num_ents = 1U << (log2size - split);
	if (num_ents > STRTAB_MAX_L1_ENTRIES) {
		dev_err(smmu->dev, "l1 entries %u exceeds max %u\n", num_ents,
			STRTAB_MAX_L1_ENTRIES);
		return -EINVAL;
	}

	size = num_ents * sizeof(struct arm_smmu_strtab_l1);
	/*
	 * HW aligns the base down to the L1 table size (min 64 bytes), so any
	 * unaligned base would make this kernel read a different table.
	 */
	if (!IS_ALIGNED(base, size)) {
		dev_err(smmu->dev, "unaligned l1 stream table base %pa\n",
			&base);
		return -EINVAL;
	}

	*num_l1_ents = num_ents;
	return 0;
}

/**
 * arm_smmu_kexec_parse_strtab_linear() - Validate a linear stream table
 * @smmu: SMMU device of this kernel
 * @cfg_reg: STRTAB_BASE_CFG register value set by the previous kernel
 * @base: stream table base address extracted from the STRTAB_BASE register
 * @num_ents: pointer to return the number of STEs
 *
 * Validate the linear stream table geometry in @cfg_reg and @base's alignment
 * against this kernel's own limits.
 *
 * Return: 0 on success with @num_ents set, or -EINVAL on a bad geometry
 */
int arm_smmu_kexec_parse_strtab_linear(struct arm_smmu_device *smmu,
				       u32 cfg_reg, phys_addr_t base,
				       u32 *num_ents)
{
	u32 log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, cfg_reg);
	unsigned int max_log2size = smmu->sid_bits;
	size_t size;

	/* Cap the size at what this kernel itself would have allocated */
	if (smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)
		max_log2size = min_t(
			unsigned int, max_log2size,
			ilog2(STRTAB_MAX_L1_ENTRIES * STRTAB_NUM_L2_STES));

	/* num_ents is limited to a u32, so cap log2size at 31 */
	max_log2size = min(max_log2size, 31U);
	if (log2size > max_log2size) {
		dev_err(smmu->dev, "unsupported log2size %u (> %u)\n", log2size,
			max_log2size);
		return -EINVAL;
	}

	size = (1U << log2size) * sizeof(struct arm_smmu_ste);
	/*
	 * HW aligns the base down to the table size, ignoring the low bits, so
	 * an unaligned base would make this kernel read a different table.
	 */
	if (!IS_ALIGNED(base, size)) {
		dev_err(smmu->dev, "unaligned stream table base %pa\n", &base);
		return -EINVAL;
	}

	*num_ents = 1U << log2size;
	return 0;
}

/**
 * arm_smmu_kexec_check_strtab_l1_desc() - Check one stream table L1 descriptor
 * @smmu: SMMU device of this kernel
 * @l1_desc: L1 descriptor value from the previous kernel's stream table
 * @idx: index of the L1 descriptor, for diagnostics
 * @l2_base: pointer to return the L2 table's physical address
 *
 * Return: 1 if the descriptor is unused, 0 if it is valid with @l2_base set, or
 * -EINVAL if it is malformed
 */
int arm_smmu_kexec_check_strtab_l1_desc(struct arm_smmu_device *smmu,
					u64 l1_desc, u32 idx,
					phys_addr_t *l2_base)
{
	phys_addr_t base = l1_desc & STRTAB_L1_DESC_L2PTR_MASK;
	u32 span = FIELD_GET(STRTAB_L1_DESC_SPAN, l1_desc);

	/* L1STD.L2Ptr is invalid */
	if (!span)
		return 1;

	if (span != STRTAB_SPLIT + 1) {
		dev_err(smmu->dev, "L1[%u] unsupported span %u (vs %u)\n", idx,
			span, STRTAB_SPLIT + 1);
		return -EINVAL;
	}

	/*
	 * A valid descriptor never carries a null pointer. Also, HW aligns the
	 * pointer down to the L2 table size, so an unaligned pointer would make
	 * this kernel read a different table.
	 */
	if (!base || !IS_ALIGNED(base, sizeof(struct arm_smmu_strtab_l2))) {
		dev_err(smmu->dev, "L1[%u] bad l2 table base %pa\n", idx,
			&base);
		return -EINVAL;
	}

	*l2_base = base;
	return 0;
}

/**
 * arm_smmu_kexec_check_ste_cdtab() - Decode the CD table geometry of an STE
 * @smmu: SMMU device of this kernel
 * @ste0: first 64 bits of the previous kernel's S1 STE
 * @cdtab: pointer to return the CD table's physical address
 * @s1fmt: pointer to return the CD table format
 * @max_contexts: pointer to return the number of CDs
 *
 * Note that a linear CD table on the 2-level capable hardware is accepted, as a
 * previous kernel might have used one, like the linear stream table.
 *
 * Also, as the CD tables are supposed to be read-only, @cdtab is not validated
 * against the table size, but only carries the field's own 64-byte alignment.
 *
 * Return: 0 on success with the three outputs set, or -EINVAL on a bad geometry
 */
int arm_smmu_kexec_check_ste_cdtab(struct arm_smmu_device *smmu, u64 ste0,
				   phys_addr_t *cdtab, u32 *s1fmt,
				   u32 *max_contexts)
{
	phys_addr_t base = ste0 & STRTAB_STE_0_S1CTXPTR_MASK;
	u32 s1cdmax = FIELD_GET(STRTAB_STE_0_S1CDMAX, ste0);
	u32 fmt = FIELD_GET(STRTAB_STE_0_S1FMT, ste0);

	if (!base || s1cdmax > smmu->ssid_bits)
		return -EINVAL;

	if (fmt != STRTAB_STE_0_S1FMT_LINEAR &&
	    fmt != STRTAB_STE_0_S1FMT_64K_L2)
		return -EINVAL;

	/* Both kernels run on the same HW, so a genuine STE never has this */
	if (fmt == STRTAB_STE_0_S1FMT_64K_L2 &&
	    !(smmu->features & ARM_SMMU_FEAT_2_LVL_CDTAB))
		return -EINVAL;

	*cdtab = base;
	*s1fmt = fmt;
	*max_contexts = 1U << s1cdmax;
	return 0;
}

/*
 * Identifies the ASID reservations of an arm_smmu_kexec_scan_and_resv_ids call.
 *
 * A failing scan can only roll back its own insertions. Two scans of the same
 * SMMU (e.g. a probe retry) must not share an ID, as the retried scan may fail
 * and roll back, while the prior one's committed reservations must persist. So
 * it must not be a per-SMMU value, but a per-scan one.
 *
 * Serialized by arm_smmu_kexec_resv_lock.
 */
static unsigned long arm_smmu_kexec_scan_id;
DEFINE_MUTEX(arm_smmu_kexec_resv_lock);

static int arm_smmu_kexec_resv_asid(struct arm_smmu_device *smmu, u32 asid)
{
	int ret;

	/* A valid CD never has ASID 0; both kernels share the same HW limit */
	if (!asid || asid >= 1UL << smmu->asid_bits)
		return -EINVAL;

	guard(mutex)(&arm_smmu_asid_lock);

	/* The value entry marks the ASID as in-use and identifies its scan */
	ret = xa_insert(&arm_smmu_asid_xa, asid,
			xa_mk_value(arm_smmu_kexec_scan_id), GFP_KERNEL);
	/*
	 * An -EBUSY against a value entry safely shares a permanent reservation
	 * made by another scan. A pointer entry means a live domain that will
	 * free its ASID for reuse eventually: keep -EBUSY to fail the scan.
	 */
	if (ret == -EBUSY && xa_is_value(xa_load(&arm_smmu_asid_xa, asid)))
		ret = 0;
	return ret;
}

static int arm_smmu_kexec_resv_vmid(struct arm_smmu_device *smmu, u32 vmid)
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

static int arm_smmu_kexec_resv_cd_asids(struct arm_smmu_device *smmu,
					struct arm_smmu_cd *cds, u32 num_cds)
{
	int ret = 0;
	u32 i;

	for (i = 0; i < num_cds; i++) {
		u64 val = le64_to_cpu(cds[i].data[0]);
		u32 asid = FIELD_GET(CTXDESC_CD_0_ASID, val);

		if (!(val & CTXDESC_CD_0_V))
			continue;
		ret = arm_smmu_kexec_resv_asid(smmu, asid);
		if (ret)
			break;
	}
	return ret;
}

/*
 * Reserve the ASIDs of all the valid CDs of an S1 STE in the previous kernel's
 * CD tables. The CD tables are transiently memremapped for the scan.
 */
static int arm_smmu_kexec_resv_s1_asids(struct arm_smmu_device *smmu, u64 ste0)
{
	struct arm_smmu_cdtab_l1 *l1tab;
	u32 num_l1_ents, num_cds, i;
	u32 max_contexts, s1fmt;
	phys_addr_t cdtab;
	int ret;

	ret = arm_smmu_kexec_check_ste_cdtab(smmu, ste0, &cdtab, &s1fmt,
					     &max_contexts);
	if (ret)
		return ret;

	if (s1fmt == STRTAB_STE_0_S1FMT_LINEAR) {
		struct arm_smmu_cd *cds;

		cds = memremap(cdtab, max_contexts * sizeof(*cds), MEMREMAP_WB);
		if (!cds)
			return -ENOMEM;
		ret = arm_smmu_kexec_resv_cd_asids(smmu, cds, max_contexts);
		memunmap(cds);
		return ret;
	}

	num_l1_ents = DIV_ROUND_UP(max_contexts, CTXDESC_L2_ENTRIES);
	l1tab = memremap(cdtab, num_l1_ents * sizeof(*l1tab), MEMREMAP_WB);
	if (!l1tab)
		return -ENOMEM;

	/* max_contexts being under a full leaf makes the only leaf partial */
	num_cds = min_t(u32, max_contexts, CTXDESC_L2_ENTRIES);

	/* Aliased L2 tables cannot extend the walk; they only repeat a scan */
	for (i = 0; i < num_l1_ents; i++) {
		u64 l1_desc = le64_to_cpu(l1tab[i].l2ptr);
		phys_addr_t l2_base = l1_desc & CTXDESC_L1_DESC_L2PTR_MASK;
		struct arm_smmu_cdtab_l2 *l2;

		if (!(l1_desc & CTXDESC_L1_DESC_V))
			continue;

		/* A valid descriptor never carries a null pointer */
		if (!l2_base) {
			ret = -EINVAL;
			break;
		}

		l2 = memremap(l2_base, num_cds * sizeof(*l2->cds), MEMREMAP_WB);
		if (!l2) {
			ret = -ENOMEM;
			break;
		}
		ret = arm_smmu_kexec_resv_cd_asids(smmu, l2->cds, num_cds);
		memunmap(l2);
		if (ret)
			break;
	}
	memunmap(l1tab);
	return ret;
}

static int arm_smmu_kexec_resv_ste_ids(struct arm_smmu_device *smmu,
				       struct arm_smmu_ste *ste)
{
	u32 vmid = FIELD_GET(STRTAB_STE_2_S2VMID, le64_to_cpu(ste->data[2]));
	u64 ste0 = le64_to_cpu(ste->data[0]);

	if (!(ste0 & STRTAB_STE_0_V))
		return 0;

	switch (FIELD_GET(STRTAB_STE_0_CFG, ste0)) {
	case STRTAB_STE_0_CFG_ABORT:
	case STRTAB_STE_0_CFG_BYPASS:
		return 0;
	case STRTAB_STE_0_CFG_S1_TRANS:
		return arm_smmu_kexec_resv_s1_asids(smmu, ste0);
	case STRTAB_STE_0_CFG_NESTED:
		/*
		 * A guest-owned CD table is in the IPA space, unreachable. Its
		 * ASIDs are only tagged with the S2VMID reserved below, so they
		 * cannot alias this kernel's VMID-0 or EL2 S1 domains.
		 */
		fallthrough;
	case STRTAB_STE_0_CFG_S2_TRANS:
		return arm_smmu_kexec_resv_vmid(smmu, vmid);
	default:
		return -EINVAL;
	}
}

/**
 * arm_smmu_kexec_scan_and_resv_ids() - Reserve a stream table's in-use IDs
 * @smmu: SMMU device of this kernel, with an adopted or restored strtab_cfg
 *
 * Scan the stream table set up in the strtab_cfg and every CD table behind an
 * S1 STE, reserving all of the in-use ASIDs and VMIDs. The caller must hold the
 * arm_smmu_kexec_resv_lock, so that a failing scan can roll back, prior to any
 * concurrent scan, via arm_smmu_kexec_unresv_ids().
 *
 * Note that the scan selects the linear or 2-level walk per this kernel's own
 * ARM_SMMU_FEAT_2_LVL_STRTAB, so the caller must have matched the feature bit
 * to the format of the adopted stream table in the strtab_cfg.
 *
 * Return: 0 on success, -EINVAL on any malformed table entry, or -ENOMEM on a
 * memory shortage
 */
int arm_smmu_kexec_scan_and_resv_ids(struct arm_smmu_device *smmu)
{
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	int ret = 0;
	u32 i, j;

	lockdep_assert_held(&arm_smmu_kexec_resv_lock);

	/* Allocate a new ID for this scan, to scope its rollback */
	arm_smmu_kexec_scan_id++;

	if (!(smmu->features & ARM_SMMU_FEAT_2_LVL_STRTAB)) {
		for (i = 0; i < cfg->linear.num_ents; i++) {
			ret = arm_smmu_kexec_resv_ste_ids(
				smmu, &cfg->linear.table[i]);
			if (ret)
				return ret;
		}
		return 0;
	}

	/* Aliased L2 tables cannot extend the scan; they only repeat a scan */
	for (i = 0; i < cfg->l2.num_l1_ents; i++) {
		u64 l1_desc = le64_to_cpu(cfg->l2.l1tab[i].l2ptr);
		struct arm_smmu_strtab_l2 *l2;
		phys_addr_t base;

		ret = arm_smmu_kexec_check_strtab_l1_desc(smmu, l1_desc, i,
							  &base);
		if (ret == 1)
			continue;
		if (ret)
			return ret;

		/*
		 * This kernel will map the previous kernel's L2 tables lazily
		 * or not at all. Here, take a transient view for this scan.
		 */
		l2 = memremap(base, sizeof(*l2), MEMREMAP_WB);
		if (!l2)
			return -ENOMEM;
		for (j = 0; j < ARRAY_SIZE(l2->stes); j++) {
			ret = arm_smmu_kexec_resv_ste_ids(smmu, &l2->stes[j]);
			if (ret)
				break;
		}
		memunmap(l2);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * arm_smmu_kexec_unresv_ids() - Roll back a failing reservation scan
 * @smmu: SMMU device of this kernel that failed its reservation scan
 *
 * Roll back the current scan for a failing arm_smmu_kexec_scan_and_resv_ids()
 * call, typically to a full reset.
 *
 * arm_smmu_kexec_resv_lock must be held for arm_smmu_kexec_scan_and_resv_ids()
 * and the roll back.
 */
void arm_smmu_kexec_unresv_ids(struct arm_smmu_device *smmu)
{
	unsigned long index;
	void *entry;

	lockdep_assert_held(&arm_smmu_kexec_resv_lock);

	mutex_lock(&arm_smmu_asid_lock);
	xa_for_each(&arm_smmu_asid_xa, index, entry) {
		if (entry == xa_mk_value(arm_smmu_kexec_scan_id))
			xa_erase(&arm_smmu_asid_xa, index);
	}
	mutex_unlock(&arm_smmu_asid_lock);

	/* No domain exists yet, so the ida holds only the reservations */
	ida_destroy(&smmu->vmid_map);
}
