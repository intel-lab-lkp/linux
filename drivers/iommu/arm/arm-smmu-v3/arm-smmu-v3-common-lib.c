// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 ARM Limited
 * Arm SMMUv3 driver functions shared with hypervisor.
 */

#include <kunit/visibility.h>

#include "arm-smmu-v3.h"
#include "arm-smmu-v3-common-lib.h"

void arm_smmu_device_iidr_probe(ARM_SMMU_OBJ *smmu)
{
	u32 reg;
	unsigned int implementer, productid, variant, revision;

	reg = readl_relaxed(smmu->base + ARM_SMMU_IIDR);
	implementer = FIELD_GET(IIDR_IMPLEMENTER, reg);
	productid = FIELD_GET(IIDR_PRODUCTID, reg);
	variant = FIELD_GET(IIDR_VARIANT, reg);
	revision = FIELD_GET(IIDR_REVISION, reg);

	switch (implementer) {
	case IIDR_IMPLEMENTER_ARM:
		switch (productid) {
		case IIDR_PRODUCTID_ARM_MMU_600:
			/* Arm erratum 1076982 */
			if (variant == 0 && revision <= 2)
				smmu->features &= ~ARM_SMMU_FEAT_SEV;
			/* Arm erratum 1209401 */
			if (variant < 2)
				smmu->features &= ~ARM_SMMU_FEAT_NESTING;
			break;
		case IIDR_PRODUCTID_ARM_MMU_700:
			/* Many errata... */
			smmu->features &= ~ARM_SMMU_FEAT_BTM;
			if (variant < 1 || revision < 1) {
				/* Arm erratum 2812531 */
				smmu->options |= ARM_SMMU_OPT_CMDQ_FORCE_SYNC;
				/* Arm errata 2268618, 2812531 */
				smmu->features &= ~ARM_SMMU_FEAT_NESTING;
			}
			break;
		case IIDR_PRODUCTID_ARM_MMU_L1:
		case IIDR_PRODUCTID_ARM_MMU_S3:
			/* Arm errata 3878312/3995052 */
			smmu->features &= ~ARM_SMMU_FEAT_BTM;
			break;
		}
		break;
	}
}

u32 arm_smmu_idr0_probe(ARM_SMMU_OBJ *smmu)
{
	u32 reg = readl_relaxed(smmu->base + ARM_SMMU_IDR0);

	/* 2-level structures */
	if (FIELD_GET(IDR0_ST_LVL, reg) == IDR0_ST_LVL_2LVL)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_STRTAB;

	if (reg & IDR0_CD2L)
		smmu->features |= ARM_SMMU_FEAT_2_LVL_CDTAB;

	/*
	 * Translation table endianness.
	 * We currently require the same endianness as the CPU, but this
	 * could be changed later by adding a new IO_PGTABLE_QUIRK.
	 */
	switch (FIELD_GET(IDR0_TTENDIAN, reg)) {
	case IDR0_TTENDIAN_MIXED:
		smmu->features |= ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE;
		break;
#ifdef __BIG_ENDIAN
	case IDR0_TTENDIAN_BE:
		smmu->features |= ARM_SMMU_FEAT_TT_BE;
		break;
#else
	case IDR0_TTENDIAN_LE:
		smmu->features |= ARM_SMMU_FEAT_TT_LE;
		break;
#endif
	}

	/* Boolean feature flags */
	if (IS_ENABLED(CONFIG_PCI_PRI) && reg & IDR0_PRI)
		smmu->features |= ARM_SMMU_FEAT_PRI;

	if (IS_ENABLED(CONFIG_PCI_ATS) && reg & IDR0_ATS)
		smmu->features |= ARM_SMMU_FEAT_ATS;

	if (reg & IDR0_SEV)
		smmu->features |= ARM_SMMU_FEAT_SEV;

	if (reg & IDR0_MSI)
		smmu->features |= ARM_SMMU_FEAT_MSI;

	if (reg & IDR0_HYP)
		smmu->features |= ARM_SMMU_FEAT_HYP;

	switch (FIELD_GET(IDR0_STALL_MODEL, reg)) {
	case IDR0_STALL_MODEL_FORCE:
		smmu->features |= ARM_SMMU_FEAT_STALL_FORCE;
		fallthrough;
	case IDR0_STALL_MODEL_STALL:
		smmu->features |= ARM_SMMU_FEAT_STALLS;
	}

	if (reg & IDR0_S1P)
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;

	if (reg & IDR0_S2P)
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;

	if ((smmu->features & ARM_SMMU_FEAT_TRANS_S1) &&
	    (smmu->features & ARM_SMMU_FEAT_TRANS_S2))
		smmu->features |= ARM_SMMU_FEAT_NESTING;

	return reg;
}

void arm_smmu_idr3_probe(ARM_SMMU_OBJ *smmu)
{
	u32 reg = readl_relaxed(smmu->base + ARM_SMMU_IDR3);

	if (FIELD_GET(IDR3_RIL, reg))
		smmu->features |= ARM_SMMU_FEAT_RANGE_INV;
	if (FIELD_GET(IDR3_FWB, reg))
		smmu->features |= ARM_SMMU_FEAT_S2FWB;
	if (FIELD_GET(IDR3_BBM, reg) == 2)
		smmu->features |= ARM_SMMU_FEAT_BBML2;
}

u32 arm_smmu_idr5_probe(ARM_SMMU_OBJ *smmu)
{
	u32 reg = readl_relaxed(smmu->base + ARM_SMMU_IDR5);

	if (reg & IDR5_DS)
		smmu->features |= ARM_SMMU_FEAT_DS;

	if (reg & IDR5_GRAN64K)
		smmu->pgsize_bitmap |= SZ_64K | SZ_512M;
	if (reg & IDR5_GRAN16K)
		smmu->pgsize_bitmap |= SZ_16K | SZ_32M;
	if (reg & IDR5_GRAN4K)
		smmu->pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;

	/* Input address size */
	if (FIELD_GET(IDR5_VAX, reg) == IDR5_VAX_52_BIT)
		smmu->features |= ARM_SMMU_FEAT_VAX;

	switch (FIELD_GET(IDR5_OAS, reg)) {
	case IDR5_OAS_32_BIT:
		smmu->oas = 32;
		break;
	case IDR5_OAS_36_BIT:
		smmu->oas = 36;
		break;
	case IDR5_OAS_40_BIT:
		smmu->oas = 40;
		break;
	case IDR5_OAS_42_BIT:
		smmu->oas = 42;
		break;
	case IDR5_OAS_44_BIT:
		smmu->oas = 44;
		break;
	case IDR5_OAS_48_BIT:
		smmu->oas = 48;
		break;
	case IDR5_OAS_52_BIT:
		smmu->oas = 52;
		smmu->pgsize_bitmap |= 1ULL << 42; /* 4TB */
		break;
	}

	return reg;
}

void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits)
{
	unsigned int cfg = FIELD_GET(STRTAB_STE_0_CFG, le64_to_cpu(ent[0]));

	used_bits[0] = cpu_to_le64(STRTAB_STE_0_V);
	if (!(ent[0] & cpu_to_le64(STRTAB_STE_0_V)))
		return;

	used_bits[0] |= cpu_to_le64(STRTAB_STE_0_CFG);

	/* S1 translates */
	if (cfg & BIT(0)) {
		used_bits[0] |= cpu_to_le64(STRTAB_STE_0_S1FMT |
					    STRTAB_STE_0_S1CTXPTR_MASK |
					    STRTAB_STE_0_S1CDMAX);
		used_bits[1] |=
			cpu_to_le64(STRTAB_STE_1_S1DSS | STRTAB_STE_1_S1CIR |
				    STRTAB_STE_1_S1COR | STRTAB_STE_1_S1CSH |
				    STRTAB_STE_1_S1STALLD | STRTAB_STE_1_STRW |
				    STRTAB_STE_1_EATS | STRTAB_STE_1_MEV);
		used_bits[2] |= cpu_to_le64(STRTAB_STE_2_S2VMID);

		/*
		 * See 13.5 Summary of attribute/permission configuration fields
		 * for the SHCFG behavior.
		 */
		if (FIELD_GET(STRTAB_STE_1_S1DSS, le64_to_cpu(ent[1])) ==
		    STRTAB_STE_1_S1DSS_BYPASS)
			used_bits[1] |= cpu_to_le64(STRTAB_STE_1_SHCFG);
	}

	/* S2 translates */
	if (cfg & BIT(1)) {
		used_bits[1] |=
			cpu_to_le64(STRTAB_STE_1_S2FWB | STRTAB_STE_1_EATS |
				    STRTAB_STE_1_SHCFG | STRTAB_STE_1_MEV);
		used_bits[2] |=
			cpu_to_le64(STRTAB_STE_2_S2VMID | STRTAB_STE_2_VTCR |
				    STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2ENDI |
				    STRTAB_STE_2_S2PTW | STRTAB_STE_2_S2S |
				    STRTAB_STE_2_S2R);
		used_bits[3] |= cpu_to_le64(STRTAB_STE_3_S2TTB_MASK);
	}

	if (cfg == STRTAB_STE_0_CFG_BYPASS)
		used_bits[1] |= cpu_to_le64(STRTAB_STE_1_SHCFG);
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_get_ste_used);

void arm_smmu_get_ste_update_safe(const __le64 *cur, const __le64 *target,
				  __le64 *safe_bits)
{
	const u64 eats_s1chk =
		FIELD_PREP(STRTAB_STE_1_EATS, STRTAB_STE_1_EATS_S1CHK);
	const u64 eats_trans =
		FIELD_PREP(STRTAB_STE_1_EATS, STRTAB_STE_1_EATS_TRANS);

	/*
	 * When an STE changes EATS_TRANS, the sequencing code in the attach
	 * logic already will have the PCI cap for ATS disabled. Thus at this
	 * moment we can expect that the device will not generate ATS queries
	 * and so we don't care about the sequencing of EATS. The purpose of
	 * EATS_TRANS is to protect the system from hostile untrusted devices
	 * that issue ATS when the PCI config space is disabled. However, if
	 * EATS_TRANS is being changed, then we must have already trusted the
	 * device as the EATS_TRANS security block is being disabled.
	 *
	 *  Note: now the EATS_TRANS update is moved to the first entry_set().
	 *  Changing S2S and EATS might transiently result in S2S=1 and EATS=1
	 *  which is a bad STE (see "5.2 Stream Table Entry"). In such a case,
	 *  we can't do a hitless update. Also, it should not be added to the
	 *  safe bits with STRTAB_STE_1_EATS_S1CHK, because EATS=0b11 would be
	 *  effectively an errant 0b00 configuration.
	 */
	if (!((cur[1] | target[1]) & cpu_to_le64(eats_s1chk)) &&
	    !((cur[2] | target[2]) & cpu_to_le64(STRTAB_STE_2_S2S)))
		safe_bits[1] |= cpu_to_le64(eats_trans);

	/*
	 * MEV does not meaningfully impact the operation of the HW, it only
	 * changes how many fault events are generated, thus we can relax it
	 * when computing the ordering. The spec notes the device can act like
	 * MEV=1 anyhow:
	 *
	 *  Note: Software must expect, and be able to deal with, coalesced
	 *  fault records even when MEV == 0.
	 */
	safe_bits[1] |= cpu_to_le64(STRTAB_STE_1_MEV);
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_get_ste_update_safe);

/*
 * Figure out if we can do a hitless update of entry to become target. Returns a
 * bit mask where 1 indicates that qword needs to be set disruptively.
 * unused_update is an intermediate value of entry that has unused bits set to
 * their new values.
 */
static u8 arm_smmu_entry_qword_diff(struct arm_smmu_entry_writer *writer,
				    const __le64 *entry, const __le64 *target,
				    __le64 *unused_update)
{
	__le64 target_used[NUM_ENTRY_QWORDS] = {};
	__le64 cur_used[NUM_ENTRY_QWORDS] = {};
	__le64 safe[NUM_ENTRY_QWORDS] = {};
	u8 used_qword_diff = 0;
	unsigned int i;

	writer->ops->get_used(entry, cur_used);
	writer->ops->get_used(target, target_used);
	if (writer->ops->get_update_safe)
		writer->ops->get_update_safe(entry, target, safe);

	for (i = 0; i != NUM_ENTRY_QWORDS; i++) {
		/*
		 * Safe is only used for bits that are used by both entries,
		 * otherwise it is sequenced according to the unused entry.
		 */
		safe[i] &= target_used[i] & cur_used[i];

		/*
		 * Check that masks are up to date, the make functions are not
		 * allowed to set a bit to 1 if the used function doesn't say it
		 * is used.
		 */
		WARN_ON_ONCE(target[i] & ~target_used[i]);

		/* Bits can change because they are not currently being used */
		cur_used[i] &= ~safe[i];
		unused_update[i] = (entry[i] & cur_used[i]) |
				   (target[i] & ~cur_used[i]);
		/*
		 * Each bit indicates that a used bit in a qword needs to be
		 * changed after unused_update is applied.
		 */
		if ((unused_update[i] & target_used[i]) != target[i])
			used_qword_diff |= 1 << i;
	}
	return used_qword_diff;
}

static void entry_set(struct arm_smmu_entry_writer *writer, __le64 *entry,
		      const __le64 *target, unsigned int start,
		      unsigned int len)
{
	bool changed = false;
	unsigned int i;

	for (i = start; len != 0; len--, i++) {
		if (entry[i] != target[i]) {
			WRITE_ONCE(entry[i], target[i]);
			changed = true;
		}
	}

	if (changed)
		writer->ops->sync(writer);
}

/*
 * Update the STE/CD to the target configuration. The transition from the
 * current entry to the target entry takes place over multiple steps that
 * attempts to make the transition hitless if possible. This function takes care
 * not to create a situation where the HW can perceive a corrupted entry. HW is
 * only required to have a 64 bit atomicity with stores from the CPU, while
 * entries are many 64 bit values big.
 *
 * The difference between the current value and the target value is analyzed to
 * determine which of three updates are required - disruptive, hitless or no
 * change.
 *
 * In the most general disruptive case we can make any update in three steps:
 *  - Disrupting the entry (V=0)
 *  - Fill now unused qwords, execpt qword 0 which contains V
 *  - Make qword 0 have the final value and valid (V=1) with a single 64
 *    bit store
 *
 * However this disrupts the HW while it is happening. There are several
 * interesting cases where a STE/CD can be updated without disturbing the HW
 * because only a small number of bits are changing (S1DSS, CONFIG, etc) or
 * because the used bits don't intersect. We can detect this by calculating how
 * many 64 bit values need update after adjusting the unused bits and skip the
 * V=0 process. This relies on the IGNORED behavior described in the
 * specification.
 */
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *entry,
			  const __le64 *target)
{
	__le64 unused_update[NUM_ENTRY_QWORDS];
	u8 used_qword_diff;

	/*
	 * Many of the entry structures have pointers to other structures that
	 * need to have their updates be visible before any writes of the entry
	 * happen.
	 */
	dma_wmb();

	used_qword_diff =
		arm_smmu_entry_qword_diff(writer, entry, target, unused_update);
	if (hweight8(used_qword_diff) == 1) {
		/*
		 * Only one qword needs its used bits to be changed. This is a
		 * hitless update, update all bits the current STE/CD is
		 * ignoring to their new values, then update a single "critical
		 * qword" to change the STE/CD and finally 0 out any bits that
		 * are now unused in the target configuration.
		 */
		unsigned int critical_qword_index = ffs(used_qword_diff) - 1;

		/*
		 * Skip writing unused bits in the critical qword since we'll be
		 * writing it in the next step anyways. This can save a sync
		 * when the only change is in that qword.
		 */
		unused_update[critical_qword_index] =
			entry[critical_qword_index];
		entry_set(writer, entry, unused_update, 0, NUM_ENTRY_QWORDS);
		entry_set(writer, entry, target, critical_qword_index, 1);
		entry_set(writer, entry, target, 0, NUM_ENTRY_QWORDS);
	} else if (used_qword_diff) {
		/*
		 * At least two qwords need their inuse bits to be changed. This
		 * requires a breaking update, zero the V bit, write all qwords
		 * but 0, then set qword 0
		 */
		unused_update[0] = 0;
		entry_set(writer, entry, unused_update, 0, 1);
		entry_set(writer, entry, target, 1, NUM_ENTRY_QWORDS - 1);
		entry_set(writer, entry, target, 0, 1);
	} else {
		/*
		 * No inuse bit changed, though safe bits may have changed.
		 */
		entry_set(writer, entry, target, 0, NUM_ENTRY_QWORDS);
	}
}
EXPORT_SYMBOL_IF_KUNIT(arm_smmu_write_entry);
