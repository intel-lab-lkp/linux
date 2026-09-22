// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 ARM Limited
 * Arm SMMUv3 driver functions shared with hypervisor.
 */

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
