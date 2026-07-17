// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm CLA driver - memory translation context
 *
 * Copyright 2026 Arm Limited.
 */

#include <asm/kvm_arm.h>

#include "arm-cla.h"

#define cla_kernel_mtc \
	(cla_kernel_pl == 2 ? CLA_MTC_REGIDX_PL2 : CLA_MTC_REGIDX_PL1)

/**
 * cla_mtc_setup - initialize the memory translation context
 * @dev: CLA device
 *
 * Return: 0 on success, or a negative error code.
 */
int cla_mtc_setup(struct cla_dev *dev)
{
	int ret;
	u64 tcr;
	u64 regs[2 * CLA_MTC_PL_SIZE] = {};
	u64 *kregs = &regs[cla_kernel_mtc];
	size_t regs_size = cla_kernel_pl * CLA_MTC_PL_SIZE;
	u64 reserved_ttbr = phys_to_ttbr(page_to_phys(ZERO_PAGE(0)));

	/* The accelerator always runs as EL0 */
	kregs[CLA_MTC_PSTATE] = FIELD_PREP(CLA_MTC_PSTATE_EL, 0) |
				FIELD_PREP(CLA_MTC_PSTATE_PAN,
					   system_uses_hw_pan());
	kregs[CLA_MTC_TTBR0] = reserved_ttbr;
	kregs[CLA_MTC_TTBR1] = reserved_ttbr;
	kregs[CLA_MTC_SCTLR] = read_sysreg(sctlr_el1);
	kregs[CLA_MTC_MAIR] = read_sysreg(mair_el1);

	if (cpus_have_final_cap(ARM64_HAS_TCR2))
		kregs[CLA_MTC_TCR2] = read_sysreg_s(SYS_TCR2_EL1);

	tcr = read_sysreg(tcr_el1);
	/*
	 * Put ASID in TTBR0, so that we only have one reg to update when
	 * switching context.
	 */
	FIELD_MODIFY(TCR_EL1_A1, &tcr, 0);
	/*
	 * We never map anything in the TTBR1 VA range so explicitly disable
	 * translations via TTBR1.
	 */
	FIELD_MODIFY(TCR_EL1_EPD1_MASK, &tcr, 1);
	kregs[CLA_MTC_TCR] = tcr;

	if (cla_kernel_pl == 2) {
		/* E2H and TGE */
		regs[CLA_MTC_HCR_EL2] = HCR_HOST_VHE_FLAGS;
	}

	/*
	 * All accelerators are idle, meaning there can't be any memory
	 * transactions happening. So it is safe to setup the MTC in any order.
	 * i.e. there is no issue with setting SCTLR.M=1 before we have
	 * configured MAIR or TCR2.
	 */
	ret = cla_op_setctx(dev, CLA_MTC_REGIDX_PL1, regs_size, regs);
	if (ret) {
		WARN_ON(cla_mtc_clear(dev));
		return ret;
	}

	return 0;
}

/**
 * cla_mtc_clear - clear the memory translation context
 * @dev: CLA device
 *
 * Return: 0 on success, or a negative error code.
 */
int cla_mtc_clear(struct cla_dev *dev)
{
	/* Don't set the RES1 bit in VTCR_EL2 because in MTC they are RAZ/WI. */
	u64 regs[2 * CLA_MTC_PL_SIZE] = {};
	size_t regs_size = cla_kernel_pl * CLA_MTC_PL_SIZE;

	/* SETCTX fails if there is no ACCID */
	if (!dev->accelerators)
		return 0;

	/*
	 * All accelerators are idle, meaning there can't be any memory
	 * transactions happening. So it is safe to clear the MTC in any order.
	 * i.e. there is no issue with clearing TTBRx before setting SCTLR.M=0.
	 */
	return cla_op_setctx(dev, CLA_MTC_REGIDX_PL1, regs_size, regs);
}

/**
 * cla_mtc_install - enable a user translation context
 * @dev: CLA device
 * @pgd: page table to install
 * @asid: ASID to use with the page table
 *
 * Return: 0 on success, or a negative error code.
 */
int cla_mtc_install(struct cla_dev *dev, pgd_t *pgd, unsigned long asid)
{
	u64 ttbr0;

	/* SETCTX fails if there is no ACCID */
	if (!dev->accelerators)
		return 0;

	ttbr0 = phys_to_ttbr(virt_to_phys(pgd)) |
		FIELD_PREP(TTBRx_EL1_ASID_MASK, asid);

	if (system_supports_cnp())
		ttbr0 |= TTBRx_EL1_CnP;

	return cla_op_setctx(dev, cla_kernel_mtc + CLA_MTC_TTBR0, 1, &ttbr0);
}

/**
 * cla_mtc_uninstall - disable the user translation context
 * @dev: CLA device
 *
 * Return: 0 on success, or a negative error code.
 */
int cla_mtc_uninstall(struct cla_dev *dev)
{
	u64 ttbr0 = phys_to_ttbr(page_to_phys(ZERO_PAGE(0)));

	/* SETCTX fails if there is no ACCID */
	if (!dev->accelerators)
		return 0;

	return cla_op_setctx(dev, cla_kernel_mtc + CLA_MTC_TTBR0, 1, &ttbr0);
}
