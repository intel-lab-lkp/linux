// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 * Arm SMMUv3 driver functions shared with hypervisor.
 */

#include "arm-smmu-v3.h"
#include <asm-generic/errno-base.h>

#include <linux/string.h>

int arm_smmu_cmdq_build_cmd(u64 *cmd, struct arm_smmu_cmdq_ent *ent)
{
	memset(cmd, 0, 1 << CMDQ_ENT_SZ_SHIFT);
	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);

	switch (ent->opcode) {
	case CMDQ_OP_TLBI_EL2_ALL:
	case CMDQ_OP_TLBI_NSNH_ALL:
		break;
	case CMDQ_OP_PREFETCH_CFG:
		cmd[0] |= FIELD_PREP(CMDQ_PREFETCH_0_SID, ent->prefetch.sid);
		break;
	case CMDQ_OP_CFGI_CD:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SSID, ent->cfgi.ssid);
		fallthrough;
	case CMDQ_OP_CFGI_STE:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);
		break;
	case CMDQ_OP_CFGI_CD_ALL:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		break;
	case CMDQ_OP_CFGI_ALL:
		/* Cover the entire SID range */
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);
		break;
	case CMDQ_OP_TLBI_NH_VA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		fallthrough;
	case CMDQ_OP_TLBI_EL2_VA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_VA_MASK;
		break;
	case CMDQ_OP_TLBI_S2_IPA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_IPA_MASK;
		break;
	case CMDQ_OP_TLBI_NH_ASID:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);
		fallthrough;
	case CMDQ_OP_TLBI_NH_ALL:
	case CMDQ_OP_TLBI_S12_VMALL:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		break;
	case CMDQ_OP_TLBI_EL2_ASID:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_ASID, ent->tlbi.asid);
		break;
	case CMDQ_OP_ATC_INV:
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_GLOBAL, ent->atc.global);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SSID, ent->atc.ssid);
		cmd[0] |= FIELD_PREP(CMDQ_ATC_0_SID, ent->atc.sid);
		cmd[1] |= FIELD_PREP(CMDQ_ATC_1_SIZE, ent->atc.size);
		cmd[1] |= ent->atc.addr & CMDQ_ATC_1_ADDR_MASK;
		break;
	case CMDQ_OP_PRI_RESP:
		cmd[0] |= FIELD_PREP(CMDQ_0_SSV, ent->substream_valid);
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SSID, ent->pri.ssid);
		cmd[0] |= FIELD_PREP(CMDQ_PRI_0_SID, ent->pri.sid);
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_GRPID, ent->pri.grpid);
		switch (ent->pri.resp) {
		case PRI_RESP_DENY:
		case PRI_RESP_FAIL:
		case PRI_RESP_SUCC:
			break;
		default:
			return -EINVAL;
		}
		cmd[1] |= FIELD_PREP(CMDQ_PRI_1_RESP, ent->pri.resp);
		break;
	case CMDQ_OP_RESUME:
		cmd[0] |= FIELD_PREP(CMDQ_RESUME_0_SID, ent->resume.sid);
		cmd[0] |= FIELD_PREP(CMDQ_RESUME_0_RESP, ent->resume.resp);
		cmd[1] |= FIELD_PREP(CMDQ_RESUME_1_STAG, ent->resume.stag);
		break;
	case CMDQ_OP_CMD_SYNC:
		if (ent->sync.msiaddr) {
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_IRQ);
			cmd[1] |= ent->sync.msiaddr & CMDQ_SYNC_1_MSIADDR_MASK;
		} else {
			cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
		}
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSH, ARM_SMMU_SH_ISH);
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_MSIATTR, ARM_SMMU_MEMATTR_OIWB);
		break;
	default:
		return -ENOENT;
	}

	return 0;
}

u32 smmu_idr0_features(u32 reg)
{
	u32 features = 0;

	/* 2-level structures */
	if (FIELD_GET(IDR0_ST_LVL, reg) == IDR0_ST_LVL_2LVL)
		features |= ARM_SMMU_FEAT_2_LVL_STRTAB;

	if (reg & IDR0_CD2L)
		features |= ARM_SMMU_FEAT_2_LVL_CDTAB;

	/*
	 * Translation table endianness.
	 * We currently require the same endianness as the CPU, but this
	 * could be changed later by adding a new IO_PGTABLE_QUIRK.
	 */
	switch (FIELD_GET(IDR0_TTENDIAN, reg)) {
	case IDR0_TTENDIAN_MIXED:
		features |= ARM_SMMU_FEAT_TT_LE | ARM_SMMU_FEAT_TT_BE;
		break;
#ifdef __BIG_ENDIAN
	case IDR0_TTENDIAN_BE:
		features |= ARM_SMMU_FEAT_TT_BE;
		break;
#else
	case IDR0_TTENDIAN_LE:
		features |= ARM_SMMU_FEAT_TT_LE;
		break;
#endif
	}

	/* Boolean feature flags */
	if (IS_ENABLED(CONFIG_PCI_PRI) && reg & IDR0_PRI)
		features |= ARM_SMMU_FEAT_PRI;

	if (IS_ENABLED(CONFIG_PCI_ATS) && reg & IDR0_ATS)
		features |= ARM_SMMU_FEAT_ATS;

	if (reg & IDR0_SEV)
		features |= ARM_SMMU_FEAT_SEV;

	if (reg & IDR0_MSI)
		features |= ARM_SMMU_FEAT_MSI;

	if (reg & IDR0_HYP)
		features |= ARM_SMMU_FEAT_HYP;

	switch (FIELD_GET(IDR0_STALL_MODEL, reg)) {
	case IDR0_STALL_MODEL_FORCE:
		features |= ARM_SMMU_FEAT_STALL_FORCE;
		fallthrough;
	case IDR0_STALL_MODEL_STALL:
		features |= ARM_SMMU_FEAT_STALLS;
	}

	if (reg & IDR0_S1P)
		features |= ARM_SMMU_FEAT_TRANS_S1;

	if (reg & IDR0_S2P)
		features |= ARM_SMMU_FEAT_TRANS_S2;

	return features;
}

u32 smmu_idr3_features(u32 reg)
{
	u32 features = 0;

	if (FIELD_GET(IDR3_RIL, reg))
		features |= ARM_SMMU_FEAT_RANGE_INV;
	if (FIELD_GET(IDR3_FWB, reg))
		features |= ARM_SMMU_FEAT_S2FWB;

	return features;
}

u32 smmu_idr5_to_oas(u32 reg)
{
	switch (FIELD_GET(IDR5_OAS, reg)) {
	case IDR5_OAS_32_BIT:
		return 32;
	case IDR5_OAS_36_BIT:
		return 36;
	case IDR5_OAS_40_BIT:
		return 40;
	case IDR5_OAS_42_BIT:
		return 42;
	case IDR5_OAS_44_BIT:
		return 44;
	case IDR5_OAS_48_BIT:
		return 48;
	case IDR5_OAS_52_BIT:
		return 52;
	}
	return 0;
}

unsigned long smmu_idr5_to_pgsize(u32 reg)
{
	unsigned long pgsize_bitmap = 0;

	if (reg & IDR5_GRAN64K)
		pgsize_bitmap |= SZ_64K | SZ_512M;
	if (reg & IDR5_GRAN16K)
		pgsize_bitmap |= SZ_16K | SZ_32M;
	if (reg & IDR5_GRAN4K)
		pgsize_bitmap |= SZ_4K | SZ_2M | SZ_1G;
	return pgsize_bitmap;
}
