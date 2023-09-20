// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synopsys DW uMCTL2 DDR ECC Driver
 * This driver is based on ppc4xx_edac.c drivers
 *
 * Copyright (C) 2012 - 2014 Xilinx, Inc.
 */

#include <linux/edac.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "edac_module.h"

/* Number of cs_rows needed per memory controller */
#define SNPS_EDAC_NR_CSROWS		1

/* Number of channels per memory controller */
#define SNPS_EDAC_NR_CHANS		1

/* Granularity of reported error in bytes */
#define SNPS_EDAC_ERR_GRAIN		1

#define SNPS_EDAC_MSG_SIZE		256

#define SNPS_EDAC_MOD_STRING		"snps_edac"
#define SNPS_EDAC_MOD_VER		"1"

/* DDR ECC Quirks */
#define SNPS_ZYNQMP_IRQ_REGS		BIT(0)

/* Synopsys uMCTL2 DDR controller registers that are relevant to ECC */

/* DDRC Master 0 Register */
#define DDR_MSTR_OFST			0x0

/* ECC Configuration Registers */
#define ECC_CFG0_OFST			0x70
#define ECC_CFG1_OFST			0x74

/* ECC Status Register */
#define ECC_STAT_OFST			0x78

/* ECC Clear Register */
#define ECC_CLR_OFST			0x7C

/* ECC Error count Register */
#define ECC_ERRCNT_OFST			0x80

/* ECC Corrected Error Address Register */
#define ECC_CEADDR0_OFST		0x84
#define ECC_CEADDR1_OFST		0x88

/* ECC Syndrome Registers */
#define ECC_CSYND0_OFST			0x8C
#define ECC_CSYND1_OFST			0x90
#define ECC_CSYND2_OFST			0x94

/* ECC Bit Mask0 Address Register */
#define ECC_BITMASK0_OFST		0x98
#define ECC_BITMASK1_OFST		0x9C
#define ECC_BITMASK2_OFST		0xA0

/* ECC UnCorrected Error Address Register */
#define ECC_UEADDR0_OFST		0xA4
#define ECC_UEADDR1_OFST		0xA8

/* ECC Syndrome Registers */
#define ECC_UESYND0_OFST		0xAC
#define ECC_UESYND1_OFST		0xB0
#define ECC_UESYND2_OFST		0xB4

/* ECC Poison Address Reg */
#define ECC_POISON0_OFST		0xB8
#define ECC_POISON1_OFST		0xBC

/* DDR Address Map Registers */
#define DDR_ADDRMAP0_OFST		0x200

/* DDR Software Control Register */
#define DDR_SWCTL			0x320

/* ZynqMP DDR QOS Registers */
#define ZYNQMP_DDR_QOS_IRQ_STAT_OFST	0x20200
#define ZYNQMP_DDR_QOS_IRQ_EN_OFST	0x20208
#define ZYNQMP_DDR_QOS_IRQ_DB_OFST	0x2020C

/* DDR Master register definitions */
#define DDR_MSTR_DEV_CFG_MASK		0xC0000000
#define DDR_MSTR_DEV_CFG_SHIFT		30
#define DDR_MSTR_DEV_X4			0
#define DDR_MSTR_DEV_X8			1
#define DDR_MSTR_DEV_X16		2
#define DDR_MSTR_DEV_X32		3
#define DDR_MSTR_BUSWIDTH_MASK		0x3000
#define DDR_MSTR_BUSWIDTH_SHIFT		12
#define DDR_MSTR_BUSWIDTH_16		2
#define DDR_MSTR_BUSWIDTH_32		1
#define DDR_MSTR_BUSWIDTH_64		0
#define DDR_MSTR_MEM_LPDDR4		0x20
#define DDR_MSTR_MEM_DDR4		0x10
#define DDR_MSTR_MEM_LPDDR3		0x8
#define DDR_MSTR_MEM_DDR2		0x4
#define DDR_MSTR_MEM_DDR3		0x1

/* ECC CFG0 register definitions */
#define ECC_CFG0_MODE_MASK		0x7
#define ECC_CFG0_MODE_SECDED		0x4

/* ECC status register definitions */
#define ECC_STAT_UECNT_MASK		0xF0000
#define ECC_STAT_UECNT_SHIFT		16
#define ECC_STAT_CECNT_MASK		0xF00
#define ECC_STAT_CECNT_SHIFT		8
#define ECC_STAT_BITNUM_MASK		0x7F

/* ECC control/clear register definitions */
#define ECC_CTRL_CLR_CE_ERR		BIT(0)
#define ECC_CTRL_CLR_UE_ERR		BIT(1)
#define ECC_CTRL_CLR_CE_ERRCNT		BIT(2)
#define ECC_CTRL_CLR_UE_ERRCNT		BIT(3)
#define ECC_CTRL_EN_CE_IRQ		BIT(8)
#define ECC_CTRL_EN_UE_IRQ		BIT(9)

/* ECC error count register definitions */
#define ECC_ERRCNT_UECNT_MASK		0xFFFF0000
#define ECC_ERRCNT_UECNT_SHIFT		16
#define ECC_ERRCNT_CECNT_MASK		0xFFFF

/* ECC Corrected Error Register Mask and Shifts*/
#define ECC_CEADDR0_RW_MASK		0x3FFFF
#define ECC_CEADDR0_RNK_MASK		BIT(24)
#define ECC_CEADDR1_BNKGRP_MASK		0x3000000
#define ECC_CEADDR1_BNKNR_MASK		0x70000
#define ECC_CEADDR1_COL_MASK		0xFFF
#define ECC_CEADDR1_BNKGRP_SHIFT	24
#define ECC_CEADDR1_BNKNR_SHIFT		16

/* ECC Poison register shifts */
#define ECC_POISON0_RANK_SHIFT		24
#define ECC_POISON0_RANK_MASK		BIT(24)
#define ECC_POISON0_COLUMN_SHIFT	0
#define ECC_POISON0_COLUMN_MASK		0xFFF
#define ECC_POISON1_BG_SHIFT		28
#define ECC_POISON1_BG_MASK		0x30000000
#define ECC_POISON1_BANKNR_SHIFT	24
#define ECC_POISON1_BANKNR_MASK		0x7000000
#define ECC_POISON1_ROW_SHIFT		0
#define ECC_POISON1_ROW_MASK		0x3FFFF

/* DDRC ECC CE & UE poison mask */
#define ECC_CEPOISON_MASK		0x3
#define ECC_UEPOISON_MASK		0x1

/* DDRC Device config shifts/masks */
#define DDR_MAX_ROW_SHIFT		18
#define DDR_MAX_COL_SHIFT		14
#define DDR_MAX_BANK_SHIFT		3
#define DDR_MAX_BANKGRP_SHIFT		2

#define ROW_MAX_VAL_MASK		0xF
#define COL_MAX_VAL_MASK		0xF
#define BANK_MAX_VAL_MASK		0x1F
#define BANKGRP_MAX_VAL_MASK		0x1F
#define RANK_MAX_VAL_MASK		0x1F

#define ROW_B0_BASE			6
#define ROW_B1_BASE			7
#define ROW_B2_BASE			8
#define ROW_B3_BASE			9
#define ROW_B4_BASE			10
#define ROW_B5_BASE			11
#define ROW_B6_BASE			12
#define ROW_B7_BASE			13
#define ROW_B8_BASE			14
#define ROW_B9_BASE			15
#define ROW_B10_BASE			16
#define ROW_B11_BASE			17
#define ROW_B12_BASE			18
#define ROW_B13_BASE			19
#define ROW_B14_BASE			20
#define ROW_B15_BASE			21
#define ROW_B16_BASE			22
#define ROW_B17_BASE			23

#define COL_B2_BASE			2
#define COL_B3_BASE			3
#define COL_B4_BASE			4
#define COL_B5_BASE			5
#define COL_B6_BASE			6
#define COL_B7_BASE			7
#define COL_B8_BASE			8
#define COL_B9_BASE			9
#define COL_B10_BASE			10
#define COL_B11_BASE			11
#define COL_B12_BASE			12
#define COL_B13_BASE			13

#define BANK_B0_BASE			2
#define BANK_B1_BASE			3
#define BANK_B2_BASE			4

#define BANKGRP_B0_BASE			2
#define BANKGRP_B1_BASE			3

#define RANK_B0_BASE			6

/* ZynqMP DDR QOS Interrupt register definitions */
#define ZYNQMP_DDR_QOS_UE_MASK		0x4
#define ZYNQMP_DDR_QOS_CE_MASK		0x2
#define ZYNQMP_DDR_QOS_IRQ_MASK		0x6

/**
 * struct snps_ecc_error_info - ECC error log information.
 * @row:	Row number.
 * @col:	Column number.
 * @bank:	Bank number.
 * @bankgrp:	Bank group number.
 * @bitpos:	Bit position.
 * @data:	Data causing the error.
 */
struct snps_ecc_error_info {
	u32 row;
	u32 col;
	u32 bank;
	u32 bankgrp;
	u32 bitpos;
	u32 data;
};

/**
 * struct snps_ecc_status - ECC status information to report.
 * @ce_cnt:	Correctable error count.
 * @ue_cnt:	Uncorrectable error count.
 * @ceinfo:	Correctable error log information.
 * @ueinfo:	Uncorrectable error log information.
 */
struct snps_ecc_status {
	u32 ce_cnt;
	u32 ue_cnt;
	struct snps_ecc_error_info ceinfo;
	struct snps_ecc_error_info ueinfo;
};

/**
 * struct snps_edac_priv - DDR memory controller private data.
 * @baseaddr:		Base address of the DDR controller.
 * @reglock:		Concurrent CSRs access lock.
 * @message:		Buffer for framing the event specific info.
 * @stat:		ECC status information.
 * @p_data:		Platform data.
 * @poison_addr:	Data poison address.
 * @row_shift:		Bit shifts for row bit.
 * @col_shift:		Bit shifts for column bit.
 * @bank_shift:		Bit shifts for bank bit.
 * @bankgrp_shift:	Bit shifts for bank group bit.
 * @rank_shift:		Bit shifts for rank bit.
 */
struct snps_edac_priv {
	void __iomem *baseaddr;
	spinlock_t reglock;
	char message[SNPS_EDAC_MSG_SIZE];
	struct snps_ecc_status stat;
	const struct snps_platform_data *p_data;
#ifdef CONFIG_EDAC_DEBUG
	ulong poison_addr;
	u32 row_shift[18];
	u32 col_shift[14];
	u32 bank_shift[3];
	u32 bankgrp_shift[2];
	u32 rank_shift[1];
#endif
};

/**
 * struct snps_platform_data - Synopsys uMCTL2 DDRC platform data.
 * @quirks:	IP-core specific quirks.
 */
struct snps_platform_data {
	u32 quirks;
};

/**
 * snps_get_error_info - Get the current ECC error info.
 * @priv:	DDR memory controller private instance data.
 *
 * Return: one if there is no error otherwise returns zero.
 */
static int snps_get_error_info(struct snps_edac_priv *priv)
{
	struct snps_ecc_status *p;
	u32 regval, clearval;
	unsigned long flags;
	void __iomem *base;

	base = priv->baseaddr;
	p = &priv->stat;

	regval = readl(base + ECC_STAT_OFST);
	if (!regval)
		return 1;

	p->ceinfo.bitpos = (regval & ECC_STAT_BITNUM_MASK);

	regval = readl(base + ECC_ERRCNT_OFST);
	p->ce_cnt = regval & ECC_ERRCNT_CECNT_MASK;
	p->ue_cnt = (regval & ECC_ERRCNT_UECNT_MASK) >> ECC_ERRCNT_UECNT_SHIFT;
	if (!p->ce_cnt)
		goto ue_err;

	regval = readl(base + ECC_CEADDR0_OFST);
	p->ceinfo.row = (regval & ECC_CEADDR0_RW_MASK);
	regval = readl(base + ECC_CEADDR1_OFST);
	p->ceinfo.bank = (regval & ECC_CEADDR1_BNKNR_MASK) >>
					ECC_CEADDR1_BNKNR_SHIFT;
	p->ceinfo.bankgrp = (regval & ECC_CEADDR1_BNKGRP_MASK) >>
					ECC_CEADDR1_BNKGRP_SHIFT;
	p->ceinfo.col = (regval & ECC_CEADDR1_COL_MASK);
	p->ceinfo.data = readl(base + ECC_CSYND0_OFST);
	edac_dbg(2, "ECCCSYN0: 0x%08X ECCCSYN1: 0x%08X ECCCSYN2: 0x%08X\n",
		 readl(base + ECC_CSYND0_OFST), readl(base + ECC_CSYND1_OFST),
		 readl(base + ECC_CSYND2_OFST));
ue_err:
	if (!p->ue_cnt)
		goto out;

	regval = readl(base + ECC_UEADDR0_OFST);
	p->ueinfo.row = (regval & ECC_CEADDR0_RW_MASK);
	regval = readl(base + ECC_UEADDR1_OFST);
	p->ueinfo.bankgrp = (regval & ECC_CEADDR1_BNKGRP_MASK) >>
					ECC_CEADDR1_BNKGRP_SHIFT;
	p->ueinfo.bank = (regval & ECC_CEADDR1_BNKNR_MASK) >>
					ECC_CEADDR1_BNKNR_SHIFT;
	p->ueinfo.col = (regval & ECC_CEADDR1_COL_MASK);
	p->ueinfo.data = readl(base + ECC_UESYND0_OFST);
out:
	spin_lock_irqsave(&priv->reglock, flags);

	clearval = readl(base + ECC_CLR_OFST) |
		   ECC_CTRL_CLR_CE_ERR | ECC_CTRL_CLR_CE_ERRCNT |
		   ECC_CTRL_CLR_UE_ERR | ECC_CTRL_CLR_UE_ERRCNT;
	writel(clearval, base + ECC_CLR_OFST);

	spin_unlock_irqrestore(&priv->reglock, flags);

	return 0;
}

/**
 * snps_handle_error - Handle Correctable and Uncorrectable errors.
 * @mci:	EDAC memory controller instance.
 * @p:		Synopsys ECC status structure.
 *
 * Handles ECC correctable and uncorrectable errors.
 */
static void snps_handle_error(struct mem_ctl_info *mci, struct snps_ecc_status *p)
{
	struct snps_edac_priv *priv = mci->pvt_info;
	struct snps_ecc_error_info *pinf;

	if (p->ce_cnt) {
		pinf = &p->ceinfo;

		snprintf(priv->message, SNPS_EDAC_MSG_SIZE,
			 "Row %d Col %d Bank %d Bank Group %d Bit %d Data 0x%08x",
			 pinf->row, pinf->col, pinf->bank, pinf->bankgrp,
			 pinf->bitpos, pinf->data);

		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci,
				     p->ce_cnt, 0, 0, 0, 0, 0, -1,
				     priv->message, "");
	}

	if (p->ue_cnt) {
		pinf = &p->ueinfo;

		snprintf(priv->message, SNPS_EDAC_MSG_SIZE,
			 "Row %d Col %d Bank %d Bank Group %d",
			 pinf->row, pinf->col, pinf->bank, pinf->bankgrp);

		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci,
				     p->ue_cnt, 0, 0, 0, 0, 0, -1,
				     priv->message, "");
	}

	memset(p, 0, sizeof(*p));
}

static void snps_enable_irq(struct snps_edac_priv *priv)
{
	unsigned long flags;

	/* Enable UE/CE Interrupts */
	if (priv->p_data->quirks & SNPS_ZYNQMP_IRQ_REGS) {
		writel(ZYNQMP_DDR_QOS_UE_MASK | ZYNQMP_DDR_QOS_CE_MASK,
		       priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_EN_OFST);

		return;
	}

	spin_lock_irqsave(&priv->reglock, flags);

	/*
	 * IRQs Enable/Disable flags have been available since v3.10a.
	 * This is noop for the older controllers.
	 */
	writel(ECC_CTRL_EN_CE_IRQ | ECC_CTRL_EN_UE_IRQ,
	       priv->baseaddr + ECC_CLR_OFST);

	spin_unlock_irqrestore(&priv->reglock, flags);
}

static void snps_disable_irq(struct snps_edac_priv *priv)
{
	unsigned long flags;

	/* Disable UE/CE Interrupts */
	if (priv->p_data->quirks & SNPS_ZYNQMP_IRQ_REGS) {
		writel(ZYNQMP_DDR_QOS_UE_MASK | ZYNQMP_DDR_QOS_CE_MASK,
		       priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_DB_OFST);

		return;
	}

	spin_lock_irqsave(&priv->reglock, flags);

	writel(0, priv->baseaddr + ECC_CLR_OFST);

	spin_unlock_irqrestore(&priv->reglock, flags);
}

/**
 * snps_irq_handler - Interrupt Handler for ECC interrupts.
 * @irq:        IRQ number.
 * @dev_id:     Device ID.
 *
 * Return: IRQ_NONE, if interrupt not set or IRQ_HANDLED otherwise.
 */
static irqreturn_t snps_irq_handler(int irq, void *dev_id)
{
	struct mem_ctl_info *mci = dev_id;
	struct snps_edac_priv *priv;
	int status, regval;

	priv = mci->pvt_info;

	if (priv->p_data->quirks & SNPS_ZYNQMP_IRQ_REGS) {
		regval = readl(priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_STAT_OFST);
		regval &= (ZYNQMP_DDR_QOS_CE_MASK | ZYNQMP_DDR_QOS_UE_MASK);
		if (!(regval & ZYNQMP_DDR_QOS_IRQ_MASK))
			return IRQ_NONE;
	}

	status = snps_get_error_info(priv);
	if (status)
		return IRQ_NONE;

	snps_handle_error(mci, &priv->stat);

	if (priv->p_data->quirks & SNPS_ZYNQMP_IRQ_REGS)
		writel(regval, priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_STAT_OFST);

	return IRQ_HANDLED;
}

/**
 * snps_get_dtype - Return the controller memory width.
 * @base:	DDR memory controller base address.
 *
 * Get the EDAC device type width appropriate for the current controller
 * configuration.
 *
 * Return: a device type width enumeration.
 */
static enum dev_type snps_get_dtype(const void __iomem *base)
{
	u32 regval;

	regval = readl(base + DDR_MSTR_OFST);
	if (!(regval & DDR_MSTR_MEM_DDR4))
		return DEV_UNKNOWN;

	regval = (regval & DDR_MSTR_DEV_CFG_MASK) >> DDR_MSTR_DEV_CFG_SHIFT;
	switch (regval) {
	case DDR_MSTR_DEV_X4:
		return DEV_X4;
	case DDR_MSTR_DEV_X8:
		return DEV_X8;
	case DDR_MSTR_DEV_X16:
		return DEV_X16;
	case DDR_MSTR_DEV_X32:
		return DEV_X32;
	}

	return DEV_UNKNOWN;
}

/**
 * snps_get_ecc_state - Return the controller ECC enable/disable status.
 * @base:	DDR memory controller base address.
 *
 * Get the ECC enable/disable status for the controller.
 *
 * Return: a ECC status boolean i.e true/false - enabled/disabled.
 */
static bool snps_get_ecc_state(void __iomem *base)
{
	u32 regval;

	regval = readl(base + ECC_CFG0_OFST) & ECC_CFG0_MODE_MASK;

	return (regval == ECC_CFG0_MODE_SECDED);
}

/**
 * snps_get_memsize - Read the size of the attached memory device.
 *
 * Return: the memory size in bytes.
 */
static u32 snps_get_memsize(void)
{
	struct sysinfo inf;

	si_meminfo(&inf);

	return inf.totalram * inf.mem_unit;
}

/**
 * snps_get_mtype - Returns controller memory type.
 * @base:	Synopsys ECC status structure.
 *
 * Get the EDAC memory type appropriate for the current controller
 * configuration.
 *
 * Return: a memory type enumeration.
 */
static enum mem_type snps_get_mtype(const void __iomem *base)
{
	enum mem_type mt;
	u32 memtype;

	memtype = readl(base + DDR_MSTR_OFST);

	if ((memtype & DDR_MSTR_MEM_DDR3) || (memtype & DDR_MSTR_MEM_LPDDR3))
		mt = MEM_DDR3;
	else if (memtype & DDR_MSTR_MEM_DDR2)
		mt = MEM_RDDR2;
	else if ((memtype & DDR_MSTR_MEM_LPDDR4) || (memtype & DDR_MSTR_MEM_DDR4))
		mt = MEM_DDR4;
	else
		mt = MEM_EMPTY;

	return mt;
}

/**
 * snps_init_csrows - Initialize the csrow data.
 * @mci:	EDAC memory controller instance.
 *
 * Initialize the chip select rows associated with the EDAC memory
 * controller instance.
 */
static void snps_init_csrows(struct mem_ctl_info *mci)
{
	struct snps_edac_priv *priv = mci->pvt_info;
	struct csrow_info *csi;
	struct dimm_info *dimm;
	u32 size, row;
	int j;

	for (row = 0; row < mci->nr_csrows; row++) {
		csi = mci->csrows[row];
		size = snps_get_memsize();

		for (j = 0; j < csi->nr_channels; j++) {
			dimm		= csi->channels[j]->dimm;
			dimm->edac_mode	= EDAC_SECDED;
			dimm->mtype	= snps_get_mtype(priv->baseaddr);
			dimm->nr_pages	= (size >> PAGE_SHIFT) / csi->nr_channels;
			dimm->grain	= SNPS_EDAC_ERR_GRAIN;
			dimm->dtype	= snps_get_dtype(priv->baseaddr);
		}
	}
}

/**
 * snps_mc_init - Initialize one driver instance.
 * @mci:	EDAC memory controller instance.
 * @pdev:	platform device.
 *
 * Perform initialization of the EDAC memory controller instance and
 * related driver-private data associated with the memory controller the
 * instance is bound to.
 */
static void snps_mc_init(struct mem_ctl_info *mci, struct platform_device *pdev)
{
	mci->pdev = &pdev->dev;
	platform_set_drvdata(pdev, mci);

	/* Initialize controller capabilities and configuration */
	mci->mtype_cap = MEM_FLAG_DDR3 | MEM_FLAG_DDR2;
	mci->edac_ctl_cap = EDAC_FLAG_NONE | EDAC_FLAG_SECDED;
	mci->scrub_cap = SCRUB_FLAG_HW_SRC;
	mci->scrub_mode = SCRUB_NONE;

	mci->edac_cap = EDAC_FLAG_SECDED;
	mci->ctl_name = "snps_umctl2_ddrc";
	mci->dev_name = SNPS_EDAC_MOD_STRING;
	mci->mod_name = SNPS_EDAC_MOD_VER;

	edac_op_state = EDAC_OPSTATE_INT;

	mci->ctl_page_to_phys = NULL;

	snps_init_csrows(mci);
}



static int snps_setup_irq(struct mem_ctl_info *mci, struct platform_device *pdev)
{
	struct snps_edac_priv *priv = mci->pvt_info;
	int ret, irq;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "No IRQ %d in DT\n", irq);
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, snps_irq_handler,
			       0, dev_name(&pdev->dev), mci);
	if (ret < 0) {
		edac_printk(KERN_ERR, EDAC_MC, "Failed to request IRQ\n");
		return ret;
	}

	snps_enable_irq(priv);

	return 0;
}

#ifdef CONFIG_EDAC_DEBUG

/**
 * snps_data_poison_setup - Update poison registers.
 * @priv:		DDR memory controller private instance data.
 *
 * Update poison registers as per DDR mapping.
 * Return: none.
 */
static void snps_data_poison_setup(struct snps_edac_priv *priv)
{
	int col = 0, row = 0, bank = 0, bankgrp = 0, rank = 0, regval;
	int index;
	ulong hif_addr = 0;

	hif_addr = priv->poison_addr >> 3;

	for (index = 0; index < DDR_MAX_ROW_SHIFT; index++) {
		if (priv->row_shift[index])
			row |= (((hif_addr >> priv->row_shift[index]) &
						BIT(0)) << index);
		else
			break;
	}

	for (index = 0; index < DDR_MAX_COL_SHIFT; index++) {
		if (priv->col_shift[index] || index < 3)
			col |= (((hif_addr >> priv->col_shift[index]) &
						BIT(0)) << index);
		else
			break;
	}

	for (index = 0; index < DDR_MAX_BANK_SHIFT; index++) {
		if (priv->bank_shift[index])
			bank |= (((hif_addr >> priv->bank_shift[index]) &
						BIT(0)) << index);
		else
			break;
	}

	for (index = 0; index < DDR_MAX_BANKGRP_SHIFT; index++) {
		if (priv->bankgrp_shift[index])
			bankgrp |= (((hif_addr >> priv->bankgrp_shift[index])
						& BIT(0)) << index);
		else
			break;
	}

	if (priv->rank_shift[0])
		rank = (hif_addr >> priv->rank_shift[0]) & BIT(0);

	regval = (rank << ECC_POISON0_RANK_SHIFT) & ECC_POISON0_RANK_MASK;
	regval |= (col << ECC_POISON0_COLUMN_SHIFT) & ECC_POISON0_COLUMN_MASK;
	writel(regval, priv->baseaddr + ECC_POISON0_OFST);

	regval = (bankgrp << ECC_POISON1_BG_SHIFT) & ECC_POISON1_BG_MASK;
	regval |= (bank << ECC_POISON1_BANKNR_SHIFT) & ECC_POISON1_BANKNR_MASK;
	regval |= (row << ECC_POISON1_ROW_SHIFT) & ECC_POISON1_ROW_MASK;
	writel(regval, priv->baseaddr + ECC_POISON1_OFST);
}

static ssize_t inject_data_error_show(struct device *dev,
				      struct device_attribute *mattr,
				      char *data)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct snps_edac_priv *priv = mci->pvt_info;

	return sprintf(data, "Poison0 Addr: 0x%08x\n\rPoison1 Addr: 0x%08x\n\r"
			"Error injection Address: 0x%lx\n\r",
			readl(priv->baseaddr + ECC_POISON0_OFST),
			readl(priv->baseaddr + ECC_POISON1_OFST),
			priv->poison_addr);
}

static ssize_t inject_data_error_store(struct device *dev,
				       struct device_attribute *mattr,
				       const char *data, size_t count)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct snps_edac_priv *priv = mci->pvt_info;

	if (kstrtoul(data, 0, &priv->poison_addr))
		return -EINVAL;

	snps_data_poison_setup(priv);

	return count;
}

static ssize_t inject_data_poison_show(struct device *dev,
				       struct device_attribute *mattr,
				       char *data)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct snps_edac_priv *priv = mci->pvt_info;

	return sprintf(data, "Data Poisoning: %s\n\r",
			(((readl(priv->baseaddr + ECC_CFG1_OFST)) & 0x3) == 0x3)
			? ("Correctable Error") : ("UnCorrectable Error"));
}

static ssize_t inject_data_poison_store(struct device *dev,
					struct device_attribute *mattr,
					const char *data, size_t count)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct snps_edac_priv *priv = mci->pvt_info;

	writel(0, priv->baseaddr + DDR_SWCTL);
	if (strncmp(data, "CE", 2) == 0)
		writel(ECC_CEPOISON_MASK, priv->baseaddr + ECC_CFG1_OFST);
	else
		writel(ECC_UEPOISON_MASK, priv->baseaddr + ECC_CFG1_OFST);
	writel(1, priv->baseaddr + DDR_SWCTL);

	return count;
}

static DEVICE_ATTR_RW(inject_data_error);
static DEVICE_ATTR_RW(inject_data_poison);

static int snps_create_sysfs_attributes(struct mem_ctl_info *mci)
{
	int rc;

	rc = device_create_file(&mci->dev, &dev_attr_inject_data_error);
	if (rc < 0)
		return rc;
	rc = device_create_file(&mci->dev, &dev_attr_inject_data_poison);
	if (rc < 0)
		return rc;
	return 0;
}

static void snps_remove_sysfs_attributes(struct mem_ctl_info *mci)
{
	device_remove_file(&mci->dev, &dev_attr_inject_data_error);
	device_remove_file(&mci->dev, &dev_attr_inject_data_poison);
}

static void snps_setup_row_address_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	u32 addrmap_row_b2_10;
	int index;

	priv->row_shift[0] = (addrmap[5] & ROW_MAX_VAL_MASK) + ROW_B0_BASE;
	priv->row_shift[1] = ((addrmap[5] >> 8) &
			ROW_MAX_VAL_MASK) + ROW_B1_BASE;

	addrmap_row_b2_10 = (addrmap[5] >> 16) & ROW_MAX_VAL_MASK;
	if (addrmap_row_b2_10 != ROW_MAX_VAL_MASK) {
		for (index = 2; index < 11; index++)
			priv->row_shift[index] = addrmap_row_b2_10 +
				index + ROW_B0_BASE;

	} else {
		priv->row_shift[2] = (addrmap[9] &
				ROW_MAX_VAL_MASK) + ROW_B2_BASE;
		priv->row_shift[3] = ((addrmap[9] >> 8) &
				ROW_MAX_VAL_MASK) + ROW_B3_BASE;
		priv->row_shift[4] = ((addrmap[9] >> 16) &
				ROW_MAX_VAL_MASK) + ROW_B4_BASE;
		priv->row_shift[5] = ((addrmap[9] >> 24) &
				ROW_MAX_VAL_MASK) + ROW_B5_BASE;
		priv->row_shift[6] = (addrmap[10] &
				ROW_MAX_VAL_MASK) + ROW_B6_BASE;
		priv->row_shift[7] = ((addrmap[10] >> 8) &
				ROW_MAX_VAL_MASK) + ROW_B7_BASE;
		priv->row_shift[8] = ((addrmap[10] >> 16) &
				ROW_MAX_VAL_MASK) + ROW_B8_BASE;
		priv->row_shift[9] = ((addrmap[10] >> 24) &
				ROW_MAX_VAL_MASK) + ROW_B9_BASE;
		priv->row_shift[10] = (addrmap[11] &
				ROW_MAX_VAL_MASK) + ROW_B10_BASE;
	}

	priv->row_shift[11] = (((addrmap[5] >> 24) & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : (((addrmap[5] >> 24) &
				ROW_MAX_VAL_MASK) + ROW_B11_BASE);
	priv->row_shift[12] = ((addrmap[6] & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : ((addrmap[6] &
				ROW_MAX_VAL_MASK) + ROW_B12_BASE);
	priv->row_shift[13] = (((addrmap[6] >> 8) & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : (((addrmap[6] >> 8) &
				ROW_MAX_VAL_MASK) + ROW_B13_BASE);
	priv->row_shift[14] = (((addrmap[6] >> 16) & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : (((addrmap[6] >> 16) &
				ROW_MAX_VAL_MASK) + ROW_B14_BASE);
	priv->row_shift[15] = (((addrmap[6] >> 24) & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : (((addrmap[6] >> 24) &
				ROW_MAX_VAL_MASK) + ROW_B15_BASE);
	priv->row_shift[16] = ((addrmap[7] & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : ((addrmap[7] &
				ROW_MAX_VAL_MASK) + ROW_B16_BASE);
	priv->row_shift[17] = (((addrmap[7] >> 8) & ROW_MAX_VAL_MASK) ==
				ROW_MAX_VAL_MASK) ? 0 : (((addrmap[7] >> 8) &
				ROW_MAX_VAL_MASK) + ROW_B17_BASE);
}

static void snps_setup_column_address_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	u32 width, memtype;
	int index;

	memtype = readl(priv->baseaddr + DDR_MSTR_OFST);
	width = (memtype & DDR_MSTR_BUSWIDTH_MASK) >> DDR_MSTR_BUSWIDTH_SHIFT;

	priv->col_shift[0] = 0;
	priv->col_shift[1] = 1;
	priv->col_shift[2] = (addrmap[2] & COL_MAX_VAL_MASK) + COL_B2_BASE;
	priv->col_shift[3] = ((addrmap[2] >> 8) &
			COL_MAX_VAL_MASK) + COL_B3_BASE;
	priv->col_shift[4] = (((addrmap[2] >> 16) & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : (((addrmap[2] >> 16) &
					COL_MAX_VAL_MASK) + COL_B4_BASE);
	priv->col_shift[5] = (((addrmap[2] >> 24) & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : (((addrmap[2] >> 24) &
					COL_MAX_VAL_MASK) + COL_B5_BASE);
	priv->col_shift[6] = ((addrmap[3] & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : ((addrmap[3] &
					COL_MAX_VAL_MASK) + COL_B6_BASE);
	priv->col_shift[7] = (((addrmap[3] >> 8) & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : (((addrmap[3] >> 8) &
					COL_MAX_VAL_MASK) + COL_B7_BASE);
	priv->col_shift[8] = (((addrmap[3] >> 16) & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : (((addrmap[3] >> 16) &
					COL_MAX_VAL_MASK) + COL_B8_BASE);
	priv->col_shift[9] = (((addrmap[3] >> 24) & COL_MAX_VAL_MASK) ==
			COL_MAX_VAL_MASK) ? 0 : (((addrmap[3] >> 24) &
					COL_MAX_VAL_MASK) + COL_B9_BASE);
	if (width == DDR_MSTR_BUSWIDTH_64) {
		if (memtype & DDR_MSTR_MEM_LPDDR3) {
			priv->col_shift[10] = ((addrmap[4] &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				((addrmap[4] & COL_MAX_VAL_MASK) +
				 COL_B10_BASE);
			priv->col_shift[11] = (((addrmap[4] >> 8) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[4] >> 8) & COL_MAX_VAL_MASK) +
				 COL_B11_BASE);
		} else {
			priv->col_shift[11] = ((addrmap[4] &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				((addrmap[4] & COL_MAX_VAL_MASK) +
				 COL_B10_BASE);
			priv->col_shift[13] = (((addrmap[4] >> 8) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[4] >> 8) & COL_MAX_VAL_MASK) +
				 COL_B11_BASE);
		}
	} else if (width == DDR_MSTR_BUSWIDTH_32) {
		if (memtype & DDR_MSTR_MEM_LPDDR3) {
			priv->col_shift[10] = (((addrmap[3] >> 24) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 24) & COL_MAX_VAL_MASK) +
				 COL_B9_BASE);
			priv->col_shift[11] = ((addrmap[4] &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				((addrmap[4] & COL_MAX_VAL_MASK) +
				 COL_B10_BASE);
		} else {
			priv->col_shift[11] = (((addrmap[3] >> 24) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 24) & COL_MAX_VAL_MASK) +
				 COL_B9_BASE);
			priv->col_shift[13] = ((addrmap[4] &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				((addrmap[4] & COL_MAX_VAL_MASK) +
				 COL_B10_BASE);
		}
	} else {
		if (memtype & DDR_MSTR_MEM_LPDDR3) {
			priv->col_shift[10] = (((addrmap[3] >> 16) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 16) & COL_MAX_VAL_MASK) +
				 COL_B8_BASE);
			priv->col_shift[11] = (((addrmap[3] >> 24) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 24) & COL_MAX_VAL_MASK) +
				 COL_B9_BASE);
		} else {
			priv->col_shift[11] = (((addrmap[3] >> 16) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 16) & COL_MAX_VAL_MASK) +
				 COL_B8_BASE);
			priv->col_shift[13] = (((addrmap[3] >> 24) &
				COL_MAX_VAL_MASK) == COL_MAX_VAL_MASK) ? 0 :
				(((addrmap[3] >> 24) & COL_MAX_VAL_MASK) +
				 COL_B9_BASE);
		}
	}

	if (width) {
		for (index = 9; index > width; index--) {
			priv->col_shift[index] = priv->col_shift[index - width];
			priv->col_shift[index - width] = 0;
		}
	}

}

static void snps_setup_bank_address_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	priv->bank_shift[0] = (addrmap[1] & BANK_MAX_VAL_MASK) + BANK_B0_BASE;
	priv->bank_shift[1] = ((addrmap[1] >> 8) &
				BANK_MAX_VAL_MASK) + BANK_B1_BASE;
	priv->bank_shift[2] = (((addrmap[1] >> 16) &
				BANK_MAX_VAL_MASK) == BANK_MAX_VAL_MASK) ? 0 :
				(((addrmap[1] >> 16) & BANK_MAX_VAL_MASK) +
				 BANK_B2_BASE);

}

static void snps_setup_bg_address_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	priv->bankgrp_shift[0] = (addrmap[8] &
				BANKGRP_MAX_VAL_MASK) + BANKGRP_B0_BASE;
	priv->bankgrp_shift[1] = (((addrmap[8] >> 8) & BANKGRP_MAX_VAL_MASK) ==
				BANKGRP_MAX_VAL_MASK) ? 0 : (((addrmap[8] >> 8)
				& BANKGRP_MAX_VAL_MASK) + BANKGRP_B1_BASE);

}

static void snps_setup_rank_address_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	priv->rank_shift[0] = ((addrmap[0] & RANK_MAX_VAL_MASK) ==
				RANK_MAX_VAL_MASK) ? 0 : ((addrmap[0] &
				RANK_MAX_VAL_MASK) + RANK_B0_BASE);
}

/**
 * snps_setup_address_map -	Set Address Map by querying ADDRMAP registers.
 * @priv:		DDR memory controller private instance data.
 *
 * Set Address Map by querying ADDRMAP registers.
 *
 * Return: none.
 */
static void snps_setup_address_map(struct snps_edac_priv *priv)
{
	u32 addrmap[12];
	int index;

	for (index = 0; index < 12; index++) {
		u32 addrmap_offset;

		addrmap_offset = DDR_ADDRMAP0_OFST + (index * 4);
		addrmap[index] = readl(priv->baseaddr + addrmap_offset);
	}

	snps_setup_row_address_map(priv, addrmap);

	snps_setup_column_address_map(priv, addrmap);

	snps_setup_bank_address_map(priv, addrmap);

	snps_setup_bg_address_map(priv, addrmap);

	snps_setup_rank_address_map(priv, addrmap);
}
#endif /* CONFIG_EDAC_DEBUG */

/**
 * snps_mc_probe - Check controller and bind driver.
 * @pdev:	platform device.
 *
 * Probe a specific controller instance for binding with the driver.
 *
 * Return: 0 if the controller instance was successfully bound to the
 * driver; otherwise, < 0 on error.
 */
static int snps_mc_probe(struct platform_device *pdev)
{
	const struct snps_platform_data *p_data;
	struct edac_mc_layer layers[2];
	struct snps_edac_priv *priv;
	struct mem_ctl_info *mci;
	void __iomem *baseaddr;
	int rc;

	baseaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(baseaddr))
		return PTR_ERR(baseaddr);

	p_data = of_device_get_match_data(&pdev->dev);
	if (!p_data)
		return -ENODEV;

	if (!snps_get_ecc_state(baseaddr)) {
		edac_printk(KERN_INFO, EDAC_MC, "ECC not enabled\n");
		return -ENXIO;
	}

	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = SNPS_EDAC_NR_CSROWS;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = SNPS_EDAC_NR_CHANS;
	layers[1].is_virt_csrow = false;

	mci = edac_mc_alloc(EDAC_AUTO_MC_NUM, ARRAY_SIZE(layers), layers,
			    sizeof(struct snps_edac_priv));
	if (!mci) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed memory allocation for mc instance\n");
		return -ENOMEM;
	}

	priv = mci->pvt_info;
	priv->baseaddr = baseaddr;
	priv->p_data = p_data;
	spin_lock_init(&priv->reglock);

	snps_mc_init(mci, pdev);

	rc = snps_setup_irq(mci, pdev);
	if (rc)
		goto free_edac_mc;

	rc = edac_mc_add_mc(mci);
	if (rc) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed to register with EDAC core\n");
		goto free_edac_mc;
	}

#ifdef CONFIG_EDAC_DEBUG
	rc = snps_create_sysfs_attributes(mci);
	if (rc) {
		edac_printk(KERN_ERR, EDAC_MC, "Failed to create sysfs entries\n");
		goto free_edac_mc;
	}

	snps_setup_address_map(priv);
#endif

	return rc;

free_edac_mc:
	edac_mc_free(mci);

	return rc;
}

/**
 * snps_mc_remove - Unbind driver from device.
 * @pdev:	Platform device.
 *
 * Return: Unconditionally 0
 */
static int snps_mc_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci = platform_get_drvdata(pdev);
	struct snps_edac_priv *priv = mci->pvt_info;

	snps_disable_irq(priv);

#ifdef CONFIG_EDAC_DEBUG
	snps_remove_sysfs_attributes(mci);
#endif

	edac_mc_del_mc(&pdev->dev);
	edac_mc_free(mci);

	return 0;
}

static const struct snps_platform_data zynqmp_edac_def = {
	.quirks = SNPS_ZYNQMP_IRQ_REGS,
};

static const struct snps_platform_data snps_edac_def = {
	.quirks = 0,
};

static const struct of_device_id snps_edac_match[] = {
	{ .compatible = "xlnx,zynqmp-ddrc-2.40a", .data = &zynqmp_edac_def },
	{ .compatible = "snps,ddrc-3.80a", .data = &snps_edac_def },
	{ }
};
MODULE_DEVICE_TABLE(of, snps_edac_match);

static struct platform_driver snps_edac_mc_driver = {
	.driver = {
		   .name = "snps-edac",
		   .of_match_table = snps_edac_match,
		   },
	.probe = snps_mc_probe,
	.remove = snps_mc_remove,
};
module_platform_driver(snps_edac_mc_driver);

MODULE_AUTHOR("Xilinx Inc");
MODULE_DESCRIPTION("Synopsys uMCTL2 DDR ECC driver");
MODULE_LICENSE("GPL v2");
