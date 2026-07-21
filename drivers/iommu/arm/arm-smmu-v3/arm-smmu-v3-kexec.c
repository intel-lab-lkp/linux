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
