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
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	u32 num_l1_ents;
	size_t size;
	int ret, i;

	ret = arm_smmu_kexec_parse_strtab_2lvl(smmu, cfg_reg, base,
					       &num_l1_ents);
	if (ret)
		return ret;

	cfg->l2.num_l1_ents = num_l1_ents;

	size = num_l1_ents * sizeof(struct arm_smmu_strtab_l1);
	cfg->l2.l1tab = memremap(base, size, MEMREMAP_WB);
	if (!cfg->l2.l1tab)
		return -ENOMEM;

	cfg->l2.l2ptrs =
		kcalloc(num_l1_ents, sizeof(*cfg->l2.l2ptrs), GFP_KERNEL);
	if (!cfg->l2.l2ptrs)
		return -ENOMEM;

	for (i = 0; i < num_l1_ents; i++) {
		u64 l2ptr = le64_to_cpu(cfg->l2.l1tab[i].l2ptr);
		phys_addr_t l2_base;

		ret = arm_smmu_kexec_check_strtab_l1_desc(smmu, l2ptr, i,
							  &l2_base);
		if (ret < 0)
			return ret;

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
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	u32 num_ents;
	size_t size;
	int ret;

	ret = arm_smmu_kexec_parse_strtab_linear(smmu, cfg_reg, base,
						 &num_ents);
	if (ret)
		return ret;

	/*
	 * We might end up with a num_ents != sid_bits, which is fine, since the
	 * ARM_SMMU_OPT_KDUMP_ADOPT case bypasses arm_smmu_write_strtab().
	 */
	cfg->linear.num_ents = num_ents;

	size = num_ents * sizeof(struct arm_smmu_ste);
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

	ret = devm_add_action_or_reset(smmu->dev, arm_smmu_kdump_adopt_cleanup,
				       smmu);
	/* devm_add_action_or_reset ran the cleanup upon failure */
	if (ret) {
		dev_warn(smmu->dev, "failed to set up cleanup action\n");
		goto err;
	}

	return 0;

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
