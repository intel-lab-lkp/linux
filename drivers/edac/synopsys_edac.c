// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synopsys DW uMCTL2 DDR ECC Driver
 * This driver is based on ppc4xx_edac.c drivers
 *
 * Copyright (C) 2012 - 2014 Xilinx, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/edac.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "edac_module.h"

/* Number of cs_rows needed per memory controller */
#define SNPS_EDAC_NR_CSROWS		1

/* Number of channels per memory controller */
#define SNPS_EDAC_NR_CHANS		1

#define SNPS_EDAC_MSG_SIZE		256

#define SNPS_EDAC_MOD_STRING		"snps_edac"
#define SNPS_EDAC_MOD_VER		"1"

/* DDR capabilities */
#define SNPS_CAP_ZYNQMP			BIT(31)

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

/* ECC Poison Pattern Registers */
#define ECC_POISONPAT0_OFST		0x37C
#define ECC_POISONPAT1_OFST		0x380
#define ECC_POISONPAT2_OFST		0x384

/* ZynqMP DDR QOS Registers */
#define ZYNQMP_DDR_QOS_IRQ_STAT_OFST	0x20200
#define ZYNQMP_DDR_QOS_IRQ_EN_OFST	0x20208
#define ZYNQMP_DDR_QOS_IRQ_DB_OFST	0x2020C

/* DDR Master register definitions */
#define DDR_MSTR_DEV_CFG_MASK		GENMASK(31, 30)
#define DDR_MSTR_DEV_X4			0
#define DDR_MSTR_DEV_X8			1
#define DDR_MSTR_DEV_X16		2
#define DDR_MSTR_DEV_X32		3
#define DDR_MSTR_ACT_RANKS_MASK		GENMASK(27, 24)
#define DDR_MSTR_FREQ_RATIO11		BIT(22)
#define DDR_MSTR_BURST_RDWR		GENMASK(19, 16)
#define DDR_MSTR_BUSWIDTH_MASK		GENMASK(13, 12)
#define DDR_MSTR_MEM_MASK		GENMASK(5, 0)
#define DDR_MSTR_MEM_LPDDR4		BIT(5)
#define DDR_MSTR_MEM_DDR4		BIT(4)
#define DDR_MSTR_MEM_LPDDR3		BIT(3)
#define DDR_MSTR_MEM_LPDDR2		BIT(2)
#define DDR_MSTR_MEM_LPDDR		BIT(1)
#define DDR_MSTR_MEM_DDR3		BIT(0)
#define DDR_MSTR_MEM_DDR2		0

/* ECC CFG0 register definitions */
#define ECC_CFG0_MODE_MASK		GENMASK(2, 0)

/* ECC status register definitions */
#define ECC_STAT_UE_MASK		GENMASK(23, 16)
#define ECC_STAT_CE_MASK		GENMASK(15, 8)
#define ECC_STAT_BITNUM_MASK		GENMASK(6, 0)

/* ECC control/clear register definitions */
#define ECC_CTRL_CLR_CE_ERR		BIT(0)
#define ECC_CTRL_CLR_UE_ERR		BIT(1)
#define ECC_CTRL_CLR_CE_ERRCNT		BIT(2)
#define ECC_CTRL_CLR_UE_ERRCNT		BIT(3)
#define ECC_CTRL_EN_CE_IRQ		BIT(8)
#define ECC_CTRL_EN_UE_IRQ		BIT(9)

/* ECC error count register definitions */
#define ECC_ERRCNT_UECNT_MASK		GENMASK(31, 16)
#define ECC_ERRCNT_CECNT_MASK		GENMASK(15, 0)

/* ECC Corrected Error register definitions */
#define ECC_CEADDR0_RANK_MASK		GENMASK(27, 24)
#define ECC_CEADDR0_ROW_MASK		GENMASK(17, 0)
#define ECC_CEADDR1_BANKGRP_MASK	GENMASK(25, 24)
#define ECC_CEADDR1_BANK_MASK		GENMASK(23, 16)
#define ECC_CEADDR1_COL_MASK		GENMASK(11, 0)

/* ECC Poison register definitions */
#define ECC_POISON0_RANK_MASK		GENMASK(27, 24)
#define ECC_POISON0_COL_MASK		GENMASK(11, 0)
#define ECC_POISON1_BANKGRP_MASK	GENMASK(29, 28)
#define ECC_POISON1_BANK_MASK		GENMASK(26, 24)
#define ECC_POISON1_ROW_MASK		GENMASK(17, 0)

/* DDRC ECC CE & UE poison mask */
#define ECC_CEPOISON_MASK		GENMASK(1, 0)
#define ECC_UEPOISON_MASK		BIT(0)

/* DDRC address mapping parameters */
#define DDR_ADDRMAP_NREGS		12

#define DDR_MAX_ROW_WIDTH		18
#define DDR_MAX_COL_WIDTH		14
#define DDR_MAX_BANK_WIDTH		3
#define DDR_MAX_BANKGRP_WIDTH		2
#define DDR_MAX_RANK_WIDTH		2

#define DDR_ADDRMAP_B0_M15		GENMASK(3, 0)
#define DDR_ADDRMAP_B8_M15		GENMASK(11, 8)
#define DDR_ADDRMAP_B16_M15		GENMASK(19, 16)
#define DDR_ADDRMAP_B24_M15		GENMASK(27, 24)

#define DDR_ADDRMAP_B0_M31		GENMASK(4, 0)
#define DDR_ADDRMAP_B8_M31		GENMASK(12, 8)
#define DDR_ADDRMAP_B16_M31		GENMASK(20, 16)
#define DDR_ADDRMAP_B24_M31		GENMASK(28, 24)

#define DDR_ADDRMAP_UNUSED		((u8)-1)
#define DDR_ADDRMAP_MAX_15		DDR_ADDRMAP_B0_M15
#define DDR_ADDRMAP_MAX_31		DDR_ADDRMAP_B0_M31

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
#define RANK_B1_BASE			7

/* ZynqMP DDR QOS Interrupt register definitions */
#define ZYNQMP_DDR_QOS_UE_MASK		BIT(2)
#define ZYNQMP_DDR_QOS_CE_MASK		BIT(1)
#define ZYNQMP_DDR_QOS_IRQ_MASK		(ZYNQMP_DDR_QOS_UE_MASK | ZYNQMP_DDR_QOS_CE_MASK)

/**
 * enum snps_dq_width - SDRAM DQ bus width (ECC capable).
 * SNPS_DQ_32:	32-bit memory data width.
 * SNPS_DQ_64:	64-bit memory data width.
 */
enum snps_dq_width {
	SNPS_DQ_32 = 2,
	SNPS_DQ_64 = 3,
};

/**
 * enum snps_dq_mode - SDRAM DQ bus mode.
 * @SNPS_DQ_FULL:	Full DQ bus width.
 * @SNPS_DQ_HALF:	Half DQ bus width.
 * @SNPS_DQ_QRTR:	Quarter DQ bus width.
 */
enum snps_dq_mode {
	SNPS_DQ_FULL = 0,
	SNPS_DQ_HALF = 1,
	SNPS_DQ_QRTR = 2,
};

/**
 * enum snps_burst_length - HIF/SDRAM burst transactions length.
 * @SNPS_DDR_BL2:	Burst length 2xSDRAM-words.
 * @SNPS_DDR_BL4:	Burst length 4xSDRAM-words.
 * @SNPS_DDR_BL8:	Burst length 8xSDRAM-words.
 * @SNPS_DDR_BL16:	Burst length 16xSDRAM-words.
 */
enum snps_burst_length {
	SNPS_DDR_BL2 = 2,
	SNPS_DDR_BL4 = 4,
	SNPS_DDR_BL8 = 8,
	SNPS_DDR_BL16 = 16,
};

/**
 * enum snps_freq_ratio - HIF:SDRAM frequency ratio mode.
 * @SNPS_FREQ_RATIO11:	1:1 frequency mode.
 * @SNPS_FREQ_RATIO12:	1:2 frequency mode.
 */
enum snps_freq_ratio {
	SNPS_FREQ_RATIO11 = 1,
	SNPS_FREQ_RATIO12 = 2,
};

/**
 * enum snps_ecc_mode - ECC mode.
 * @SNPS_ECC_DISABLED:	ECC is disabled/unavailable.
 * @SNPS_ECC_SECDED:	SEC/DED over 1 beat ECC (SideBand/Inline).
 * @SNPS_ECC_ADVX4X8:	Advanced ECC X4/X8 (SideBand).
 */
enum snps_ecc_mode {
	SNPS_ECC_DISABLED = 0,
	SNPS_ECC_SECDED = 4,
	SNPS_ECC_ADVX4X8 = 5,
};

/**
 * struct snps_ddrc_info - DDR controller platform parameters.
 * @caps:		DDR controller capabilities.
 * @sdram_mode:		Current SDRAM mode selected.
 * @dev_cfg:		Current memory device config (if applicable).
 * @dq_width:		Memory data bus width (width of the DQ signals
 *			connected to SDRAM chips).
 * @dq_mode:		Proportion of the DQ bus utilized to access SDRAM.
 * @sdram_burst_len:	SDRAM burst transaction length.
 * @hif_burst_len:	HIF burst transaction length (Host Interface).
 * @freq_ratio:		HIF/SDRAM frequency ratio mode.
 * @ecc_mode:		ECC mode enabled for the DDR controller (SEC/DED, etc).
 * @ranks:		Number of ranks enabled to access DIMM (1, 2 or 4).
 */
struct snps_ddrc_info {
	unsigned int caps;
	enum mem_type sdram_mode;
	enum dev_type dev_cfg;
	enum snps_dq_width dq_width;
	enum snps_dq_mode dq_mode;
	enum snps_burst_length sdram_burst_len;
	enum snps_burst_length hif_burst_len;
	enum snps_freq_ratio freq_ratio;
	enum snps_ecc_mode ecc_mode;
	unsigned int ranks;
};

/**
 * struct snps_hif_sdram_map - HIF/SDRAM mapping table.
 * @row:	HIF bit offsets used as row address bits.
 * @col:	HIF bit offsets used as column address bits.
 * @bank:	HIF bit offsets used as bank address bits.
 * @bankgrp:	HIF bit offsets used as bank group address bits.
 * @rank:	HIF bit offsets used as rank address bits.
 *
 * For example, row[0] = 6 means row bit #0 is encoded by the HIF
 * address bit #6 and vice-versa.
 */
struct snps_hif_sdram_map {
	u8 row[DDR_MAX_ROW_WIDTH];
	u8 col[DDR_MAX_COL_WIDTH];
	u8 bank[DDR_MAX_BANK_WIDTH];
	u8 bankgrp[DDR_MAX_BANKGRP_WIDTH];
	u8 rank[DDR_MAX_RANK_WIDTH];
};

/**
 * struct snps_sdram_addr - SDRAM address.
 * @row:	Row number.
 * @col:	Column number.
 * @bank:	Bank number.
 * @bankgrp:	Bank group number.
 * @rank:	Rank number.
 */
struct snps_sdram_addr {
	u16 row;
	u16 col;
	u8 bank;
	u8 bankgrp;
	u8 rank;
};

/**
 * struct snps_ecc_error_info - ECC error log information.
 * @row:	Row number.
 * @col:	Column number.
 * @bank:	Bank number.
 * @bankgrp:	Bank group number.
 * @syndrome:	Error syndrome.
 * @bitpos:	Bit position.
 * @data:	Data causing the error.
 * @ecc:	Data ECC.
 */
struct snps_ecc_error_info {
	u32 row;
	u32 col;
	u32 bank;
	u32 bankgrp;
	u32 syndrome;
	u32 bitpos;
	u64 data;
	u32 ecc;
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
 * @info:		DDR controller config info.
 * @hif_sdram_map:	HIF/SDRAM mapping table.
 * @pdev:		Platform device.
 * @baseaddr:		Base address of the DDR controller.
 * @reglock:		Concurrent CSRs access lock.
 * @message:		Buffer for framing the event specific info.
 * @stat:		ECC status information.
 * @poison_addr:	Data poison address.
 */
struct snps_edac_priv {
	struct snps_ddrc_info info;
	struct snps_hif_sdram_map hif_sdram_map;
	struct platform_device *pdev;
	void __iomem *baseaddr;
	spinlock_t reglock;
	char message[SNPS_EDAC_MSG_SIZE];
	struct snps_ecc_status stat;
#ifdef CONFIG_EDAC_DEBUG
	ulong poison_addr;
#endif
};

/**
 * snps_map_app_to_hif - Map Application address to HIF address.
 * @priv:	DDR memory controller private instance data.
 * @app:	Application address (source).
 * @hif:	HIF address (destination).
 *
 * HIF address is used to perform the DQ bus width aligned burst transactions.
 * So in order to perform the Application-to-HIF address translation we just
 * need to discard the SDRAM-word bits of the Application address.
 */
static void snps_map_app_to_hif(struct snps_edac_priv *priv,
				u64 app, u64 *hif)
{
	*hif = app >> priv->info.dq_width;
}

/**
 * snps_map_hif_to_sdram - Map HIF address to SDRAM address.
 * @priv:	DDR memory controller private instance data.
 * @hif:	HIF address (source).
 * @sdram:	SDRAM address (destination).
 *
 * HIF-SDRAM address mapping is configured with the ADDRMAPx registers, Based
 * on the CSRs value the HIF address bits are mapped to the corresponding bits
 * in the SDRAM rank/bank/column/row. If an SDRAM address bit is unused (there
 * is no any HIF address bit corresponding to it) it will be set to zero. Using
 * this fact we can freely set the output SDRAM address with zeros and walk
 * over the set HIF address bits only. Similarly the unmapped HIF address bits
 * are just ignored.
 */
static void snps_map_hif_to_sdram(struct snps_edac_priv *priv,
				  u64 hif, struct snps_sdram_addr *sdram)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	int i;

	sdram->row = 0;
	for (i = 0; i < DDR_MAX_ROW_WIDTH; i++) {
		if (map->row[i] != DDR_ADDRMAP_UNUSED && hif & BIT(map->row[i]))
			sdram->row |= BIT(i);
	}

	sdram->col = 0;
	for (i = 0; i < DDR_MAX_COL_WIDTH; i++) {
		if (map->col[i] != DDR_ADDRMAP_UNUSED && hif & BIT(map->col[i]))
			sdram->col |= BIT(i);
	}

	sdram->bank = 0;
	for (i = 0; i < DDR_MAX_BANK_WIDTH; i++) {
		if (map->bank[i] != DDR_ADDRMAP_UNUSED && hif & BIT(map->bank[i]))
			sdram->bank |= BIT(i);
	}

	sdram->bankgrp = 0;
	for (i = 0; i < DDR_MAX_BANKGRP_WIDTH; i++) {
		if (map->bankgrp[i] != DDR_ADDRMAP_UNUSED && hif & BIT(map->bankgrp[i]))
			sdram->bankgrp |= BIT(i);
	}

	sdram->rank = 0;
	for (i = 0; i < DDR_MAX_RANK_WIDTH; i++) {
		if (map->rank[i] != DDR_ADDRMAP_UNUSED && hif & BIT(map->rank[i]))
			sdram->rank |= BIT(i);
	}
}

/**
 * snps_map_sys_to_sdram - Map System address to SDRAM address.
 * @priv:	DDR memory controller private instance data.
 * @sys:	System address (source).
 * @sdram:	SDRAM address (destination).
 *
 * Perform a full mapping of the system address (detected on the controller
 * ports) to the SDRAM address tuple row/column/bank/etc.
 */
static void snps_map_sys_to_sdram(struct snps_edac_priv *priv,
				  dma_addr_t sys, struct snps_sdram_addr *sdram)
{
	u64 app, hif;

	app = sys;

	snps_map_app_to_hif(priv, app, &hif);

	snps_map_hif_to_sdram(priv, hif, sdram);
}

/**
 * snps_get_bitpos - Get DQ-bus corrected bit position.
 * @syndrome:	Error syndrome.
 * @dq_width:	Controller DQ-bus width.
 *
 * Return: actual corrected DQ-bus bit position starting from 0.
 */
static inline u32 snps_get_bitpos(u32 syndrome, enum snps_dq_width dq_width)
{
	/* ecc[0] bit */
	if (syndrome == 0)
		return BITS_PER_BYTE << dq_width;

	/* ecc[1:x] bit */
	if (is_power_of_2(syndrome))
		return (BITS_PER_BYTE << dq_width) + ilog2(syndrome) + 1;

	/* data[0:y] bit */
	return syndrome - ilog2(syndrome) - 2;
}

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

	p->ceinfo.syndrome = FIELD_GET(ECC_STAT_BITNUM_MASK, regval);

	regval = readl(base + ECC_ERRCNT_OFST);
	p->ce_cnt = FIELD_GET(ECC_ERRCNT_CECNT_MASK, regval);
	p->ue_cnt = FIELD_GET(ECC_ERRCNT_UECNT_MASK, regval);
	if (!p->ce_cnt)
		goto ue_err;

	p->ceinfo.bitpos = snps_get_bitpos(p->ceinfo.syndrome, priv->info.dq_width);

	regval = readl(base + ECC_CEADDR0_OFST);
	p->ceinfo.row = FIELD_GET(ECC_CEADDR0_ROW_MASK, regval);

	regval = readl(base + ECC_CEADDR1_OFST);
	p->ceinfo.bank = FIELD_GET(ECC_CEADDR1_BANK_MASK, regval);
	p->ceinfo.bankgrp = FIELD_GET(ECC_CEADDR1_BANKGRP_MASK, regval);
	p->ceinfo.col = FIELD_GET(ECC_CEADDR1_COL_MASK, regval);

	p->ceinfo.data = readl(base + ECC_CSYND0_OFST);
	if (priv->info.dq_width == SNPS_DQ_64)
		p->ceinfo.data |= (u64)readl(base + ECC_CSYND1_OFST) << 32;

	p->ceinfo.ecc = readl(base + ECC_CSYND2_OFST);

ue_err:
	if (!p->ue_cnt)
		goto out;

	regval = readl(base + ECC_UEADDR0_OFST);
	p->ueinfo.row = FIELD_GET(ECC_CEADDR0_ROW_MASK, regval);

	regval = readl(base + ECC_UEADDR1_OFST);
	p->ueinfo.bankgrp = FIELD_GET(ECC_CEADDR1_BANKGRP_MASK, regval);
	p->ueinfo.bank = FIELD_GET(ECC_CEADDR1_BANK_MASK, regval);
	p->ueinfo.col = FIELD_GET(ECC_CEADDR1_COL_MASK, regval);

	p->ueinfo.data = readl(base + ECC_UESYND0_OFST);
	if (priv->info.dq_width == SNPS_DQ_64)
		p->ueinfo.data |= (u64)readl(base + ECC_UESYND1_OFST) << 32;

	p->ueinfo.ecc = readl(base + ECC_UESYND2_OFST);

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
			 "Row %d Col %d Bank %d Bank Group %d Bit %d Data 0x%08llx:0x%02x",
			 pinf->row, pinf->col, pinf->bank, pinf->bankgrp,
			 pinf->bitpos, pinf->data, pinf->ecc);

		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci,
				     p->ce_cnt, 0, 0, pinf->syndrome, 0, 0, -1,
				     priv->message, "");
	}

	if (p->ue_cnt) {
		pinf = &p->ueinfo;

		snprintf(priv->message, SNPS_EDAC_MSG_SIZE,
			 "Row %d Col %d Bank %d Bank Group %d Data 0x%08llx:0x%02x",
			 pinf->row, pinf->col, pinf->bank, pinf->bankgrp,
			 pinf->data, pinf->ecc);

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
	if (priv->info.caps & SNPS_CAP_ZYNQMP) {
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
	if (priv->info.caps & SNPS_CAP_ZYNQMP) {
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

	if (priv->info.caps & SNPS_CAP_ZYNQMP) {
		regval = readl(priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_STAT_OFST);
		regval &= (ZYNQMP_DDR_QOS_CE_MASK | ZYNQMP_DDR_QOS_UE_MASK);
		if (!(regval & ZYNQMP_DDR_QOS_IRQ_MASK))
			return IRQ_NONE;
	}

	status = snps_get_error_info(priv);
	if (status)
		return IRQ_NONE;

	snps_handle_error(mci, &priv->stat);

	if (priv->info.caps & SNPS_CAP_ZYNQMP)
		writel(regval, priv->baseaddr + ZYNQMP_DDR_QOS_IRQ_STAT_OFST);

	return IRQ_HANDLED;
}

/**
 * snps_create_data - Create private data.
 * @pdev:	platform device.
 *
 * Return: Private data instance or negative errno.
 */
static struct snps_edac_priv *snps_create_data(struct platform_device *pdev)
{
	struct snps_edac_priv *priv;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	priv->baseaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->baseaddr))
		return ERR_CAST(priv->baseaddr);

	priv->pdev = pdev;
	spin_lock_init(&priv->reglock);

	return priv;
}

/*
 * zynqmp_init_plat - ZynqMP-specific platform initialization.
 * @priv:	DDR memory controller private data.
 *
 * Return: always zero.
 */
static int zynqmp_init_plat(struct snps_edac_priv *priv)
{
	priv->info.caps |= SNPS_CAP_ZYNQMP;
	priv->info.dq_width = SNPS_DQ_64;

	return 0;
}

/**
 * snps_get_dtype - Return the controller memory width.
 * @mstr:	Master CSR value.
 *
 * Get the EDAC device type width appropriate for the current controller
 * configuration.
 *
 * Return: a device type width enumeration.
 */
static inline enum dev_type snps_get_dtype(u32 mstr)
{
	if (!(mstr & DDR_MSTR_MEM_DDR4))
		return DEV_UNKNOWN;

	switch (FIELD_GET(DDR_MSTR_DEV_CFG_MASK, mstr)) {
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
 * @mstr:	Master CSR value.
 *
 * Get the EDAC memory type appropriate for the current controller
 * configuration.
 *
 * Return: a memory type enumeration.
 */
static inline enum mem_type snps_get_mtype(u32 mstr)
{
	switch (FIELD_GET(DDR_MSTR_MEM_MASK, mstr)) {
	case DDR_MSTR_MEM_DDR2:
		return MEM_DDR2;
	case DDR_MSTR_MEM_DDR3:
		return MEM_DDR3;
	case DDR_MSTR_MEM_LPDDR:
		return MEM_LPDDR;
	case DDR_MSTR_MEM_LPDDR2:
		return MEM_LPDDR2;
	case DDR_MSTR_MEM_LPDDR3:
		return MEM_LPDDR3;
	case DDR_MSTR_MEM_DDR4:
		return MEM_DDR4;
	case DDR_MSTR_MEM_LPDDR4:
		return MEM_LPDDR4;
	}

	return MEM_RESERVED;
}

/**
 * snps_get_ddrc_info - Get the DDR controller config data.
 * @priv:	DDR memory controller private data.
 *
 * Return: negative errno if no ECC detected, otherwise - zero.
 */
static int snps_get_ddrc_info(struct snps_edac_priv *priv)
{
	int (*init_plat)(struct snps_edac_priv *priv);
	u32 regval;

	/* Before getting the DDRC parameters make sure ECC is enabled */
	regval = readl(priv->baseaddr + ECC_CFG0_OFST);

	priv->info.ecc_mode = FIELD_GET(ECC_CFG0_MODE_MASK, regval);
	if (priv->info.ecc_mode != SNPS_ECC_SECDED) {
		edac_printk(KERN_INFO, EDAC_MC, "SEC/DED ECC not enabled\n");
		return -ENXIO;
	}

	/* Auto-detect the basic HIF/SDRAM bus parameters */
	regval = readl(priv->baseaddr + DDR_MSTR_OFST);

	priv->info.sdram_mode = snps_get_mtype(regval);
	priv->info.dev_cfg = snps_get_dtype(regval);

	priv->info.dq_mode = FIELD_GET(DDR_MSTR_BUSWIDTH_MASK, regval);

	/*
	 * Assume HIF burst length matches the SDRAM burst length since it's
	 * not auto-detectable
	 */
	priv->info.sdram_burst_len = FIELD_GET(DDR_MSTR_BURST_RDWR, regval) << 1;
	priv->info.hif_burst_len = priv->info.sdram_burst_len;

	/* Retrieve the current HIF/SDRAM frequency ratio: 1:1 vs 1:2 */
	priv->info.freq_ratio = !(regval & DDR_MSTR_FREQ_RATIO11) + 1;

	/* Activated ranks field: set bit corresponds to populated rank */
	priv->info.ranks = FIELD_GET(DDR_MSTR_ACT_RANKS_MASK, regval);
	priv->info.ranks = hweight_long(priv->info.ranks);

	/* Auto-detect the DQ bus width by using the ECC-poison pattern CSR */
	writel(0, priv->baseaddr + DDR_SWCTL);

	/*
	 * If poison pattern [32:64] is changeable then DQ is 64-bit wide.
	 * Note the feature has been available since IP-core v2.51a.
	 */
	regval = readl(priv->baseaddr + ECC_POISONPAT1_OFST);
	writel(~regval, priv->baseaddr + ECC_POISONPAT1_OFST);
	if (regval != readl(priv->baseaddr + ECC_POISONPAT1_OFST)) {
		priv->info.dq_width = SNPS_DQ_64;
		writel(regval, priv->baseaddr + ECC_POISONPAT1_OFST);
	} else {
		priv->info.dq_width = SNPS_DQ_32;
	}

	writel(1, priv->baseaddr + DDR_SWCTL);

	/* Apply platform setups after all the configs auto-detection */
	init_plat = device_get_match_data(&priv->pdev->dev);

	return init_plat ? init_plat(priv) : 0;
}

/**
 * snps_get_hif_row_map - Get HIF/SDRAM-row address map.
 * @priv:	DDR memory controller private instance data.
 * @addrmap:	Array with ADDRMAP registers value.
 *
 * SDRAM-row address is defined by the fields in the ADDRMAP[5-7,9-11]
 * registers. Those fields value indicate the HIF address bits used to encode
 * the DDR row address.
 */
static void snps_get_hif_row_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	u8 map_row_b2_10;
	int i;

	for (i = 0; i < DDR_MAX_ROW_WIDTH; i++)
		map->row[i] = DDR_ADDRMAP_UNUSED;

	map->row[0] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[5]) + ROW_B0_BASE;
	map->row[1] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[5]) + ROW_B1_BASE;

	map_row_b2_10 = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[5]);
	if (map_row_b2_10 != DDR_ADDRMAP_MAX_15) {
		for (i = 2; i < 11; i++)
			map->row[i] = map_row_b2_10 + i + ROW_B0_BASE;
	} else {
		map->row[2] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[9]) + ROW_B2_BASE;
		map->row[3] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[9]) + ROW_B3_BASE;
		map->row[4] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[9]) + ROW_B4_BASE;
		map->row[5] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[9]) + ROW_B5_BASE;
		map->row[6] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[10]) + ROW_B6_BASE;
		map->row[7] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[10]) + ROW_B7_BASE;
		map->row[8] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[10]) + ROW_B8_BASE;
		map->row[9] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[10]) + ROW_B9_BASE;
		map->row[10] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[11]) + ROW_B10_BASE;
	}

	map->row[11] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[5]);
	map->row[11] = map->row[11] == DDR_ADDRMAP_MAX_15 ?
		       DDR_ADDRMAP_UNUSED : map->row[11] + ROW_B11_BASE;

	map->row[12] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[6]);
	map->row[12] = map->row[12] == DDR_ADDRMAP_MAX_15 ?
		       DDR_ADDRMAP_UNUSED : map->row[12] + ROW_B12_BASE;

	map->row[13] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[6]);
	map->row[13] = map->row[13] == DDR_ADDRMAP_MAX_15 ?
		       DDR_ADDRMAP_UNUSED : map->row[13] + ROW_B13_BASE;

	map->row[14] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[6]);
	map->row[14] = map->row[14] == DDR_ADDRMAP_MAX_15 ?
		       DDR_ADDRMAP_UNUSED : map->row[14] + ROW_B14_BASE;

	map->row[15] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[6]);
	map->row[15] = map->row[15] == DDR_ADDRMAP_MAX_15 ?
		       DDR_ADDRMAP_UNUSED : map->row[15] + ROW_B15_BASE;

	if (priv->info.sdram_mode == MEM_DDR4 || priv->info.sdram_mode == MEM_LPDDR4) {
		map->row[16] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[7]);
		map->row[16] = map->row[16] == DDR_ADDRMAP_MAX_15 ?
			       DDR_ADDRMAP_UNUSED : map->row[16] + ROW_B16_BASE;

		map->row[17] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[7]);
		map->row[17] = map->row[17] == DDR_ADDRMAP_MAX_15 ?
			       DDR_ADDRMAP_UNUSED : map->row[17] + ROW_B17_BASE;
	}
}

/**
 * snps_get_hif_col_map - Get HIF/SDRAM-column address map.
 * @priv:	DDR memory controller private instance data.
 * @addrmap:	Array with ADDRMAP registers value.
 *
 * SDRAM-column address is defined by the fields in the ADDRMAP[2-4]
 * registers. Those fields value indicate the HIF address bits used to encode
 * the DDR row address.
 */
static void snps_get_hif_col_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	int i;

	for (i = 0; i < DDR_MAX_COL_WIDTH; i++)
		map->col[i] = DDR_ADDRMAP_UNUSED;

	map->col[0] = 0;
	map->col[1] = 1;
	map->col[2] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[2]) + COL_B2_BASE;
	map->col[3] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[2]) + COL_B3_BASE;

	map->col[4] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[2]);
	map->col[4] = map->col[4] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[4] + COL_B4_BASE;

	map->col[5] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[2]);
	map->col[5] = map->col[5] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[5] + COL_B5_BASE;

	map->col[6] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[3]);
	map->col[6] = map->col[6] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[6] + COL_B6_BASE;

	map->col[7] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[3]);
	map->col[7] = map->col[7] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[7] + COL_B7_BASE;

	map->col[8] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[3]);
	map->col[8] = map->col[8] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[8] + COL_B8_BASE;

	map->col[9] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[3]);
	map->col[9] = map->col[9] == DDR_ADDRMAP_MAX_15 ?
		      DDR_ADDRMAP_UNUSED : map->col[9] + COL_B9_BASE;

	if (priv->info.dq_mode) {
		for (i = 9; i > priv->info.dq_mode; i--) {
			map->col[i] = map->col[i - priv->info.dq_mode];
			map->col[i - priv->info.dq_mode] = DDR_ADDRMAP_UNUSED;
		}
	}

	/*
	 * Per JEDEC DDR2/3/4/mDDR specification, column address bit 10 is
	 * reserved for indicating auto-precharge, and hence no source
	 * address bit can be mapped to col[10].
	 * Per JEDEC specification, column address bit 12 is reserved
	 * for the Burst-chop status, so no source address bit mapping
	 * for col[12] either.
	 */
	if (priv->info.dq_mode == SNPS_DQ_FULL) {
		if (priv->info.sdram_mode == MEM_LPDDR3) {
			map->col[10] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[4]);
			map->col[10] = map->col[10] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[10] + COL_B10_BASE;

			map->col[11] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[4]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B11_BASE;
		} else {
			map->col[11] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[4]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B10_BASE;

			map->col[13] = FIELD_GET(DDR_ADDRMAP_B8_M15, addrmap[4]);
			map->col[13] = map->col[13] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[13] + COL_B11_BASE;
		}
	} else if (priv->info.dq_mode == SNPS_DQ_HALF) {
		if (priv->info.sdram_mode == MEM_LPDDR3) {
			map->col[10] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[3]);
			map->col[10] = map->col[10] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[10] + COL_B9_BASE;

			map->col[11] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[4]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B10_BASE;
		} else {
			map->col[11] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[3]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B9_BASE;

			map->col[13] = FIELD_GET(DDR_ADDRMAP_B0_M15, addrmap[4]);
			map->col[13] = map->col[13] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[13] + COL_B10_BASE;
		}
	} else {
		if (priv->info.sdram_mode == MEM_LPDDR3) {
			map->col[10] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[3]);
			map->col[10] = map->col[10] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[10] + COL_B8_BASE;

			map->col[11] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[3]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B9_BASE;
		} else {
			map->col[11] = FIELD_GET(DDR_ADDRMAP_B16_M15, addrmap[3]);
			map->col[11] = map->col[11] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[11] + COL_B8_BASE;

			map->col[11] = FIELD_GET(DDR_ADDRMAP_B24_M15, addrmap[3]);
			map->col[13] = map->col[13] == DDR_ADDRMAP_MAX_15 ?
				       DDR_ADDRMAP_UNUSED : map->col[13] + COL_B9_BASE;
		}
	}
}

/**
 * snps_get_hif_bank_map - Get HIF/SDRAM-bank address map.
 * @priv:	DDR memory controller private instance data.
 * @addrmap:	Array with ADDRMAP registers value.
 *
 * SDRAM-bank address is defined by the fields in the ADDRMAP[1]
 * register. Those fields value indicate the HIF address bits used to encode
 * the DDR bank address.
 */
static void snps_get_hif_bank_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	int i;

	for (i = 0; i < DDR_MAX_BANK_WIDTH; i++)
		map->bank[i] = DDR_ADDRMAP_UNUSED;

	map->bank[0] = FIELD_GET(DDR_ADDRMAP_B0_M31, addrmap[1]) + BANK_B0_BASE;
	map->bank[1] = FIELD_GET(DDR_ADDRMAP_B8_M31, addrmap[1]) + BANK_B1_BASE;

	map->bank[2] = FIELD_GET(DDR_ADDRMAP_B16_M31, addrmap[1]);
	map->bank[2] = map->bank[2] == DDR_ADDRMAP_MAX_31 ?
		       DDR_ADDRMAP_UNUSED : map->bank[2] + BANK_B2_BASE;
}

/**
 * snps_get_hif_bankgrp_map - Get HIF/SDRAM-bank group address map.
 * @priv:	DDR memory controller private instance data.
 * @addrmap:	Array with ADDRMAP registers value.
 *
 * SDRAM-bank group address is defined by the fields in the ADDRMAP[8]
 * register. Those fields value indicate the HIF address bits used to encode
 * the DDR bank group address.
 */
static void snps_get_hif_bankgrp_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	int i;

	for (i = 0; i < DDR_MAX_BANKGRP_WIDTH; i++)
		map->bankgrp[i] = DDR_ADDRMAP_UNUSED;

	/* Bank group signals are available on the DDR4 memory only */
	if (priv->info.sdram_mode != MEM_DDR4)
		return;

	map->bankgrp[0] = FIELD_GET(DDR_ADDRMAP_B0_M31, addrmap[8]) + BANKGRP_B0_BASE;

	map->bankgrp[1] = FIELD_GET(DDR_ADDRMAP_B8_M31, addrmap[8]);
	map->bankgrp[1] = map->bankgrp[1] == DDR_ADDRMAP_MAX_31 ?
			  DDR_ADDRMAP_UNUSED : map->bankgrp[1] + BANKGRP_B1_BASE;
}

/**
 * snps_get_hif_rank_map - Get HIF/SDRAM-rank address map.
 * @priv:	DDR memory controller private instance data.
 * @addrmap:	Array with ADDRMAP registers value.
 *
 * SDRAM-rank address is defined by the fields in the ADDRMAP[0]
 * register. Those fields value indicate the HIF address bits used to encode
 * the DDR rank address.
 */
static void snps_get_hif_rank_map(struct snps_edac_priv *priv, u32 *addrmap)
{
	struct snps_hif_sdram_map *map = &priv->hif_sdram_map;
	int i;

	for (i = 0; i < DDR_MAX_RANK_WIDTH; i++)
		map->rank[i] = DDR_ADDRMAP_UNUSED;

	if (priv->info.ranks > 1) {
		map->rank[0] = FIELD_GET(DDR_ADDRMAP_B0_M31, addrmap[0]);
		map->rank[0] = map->rank[0] == DDR_ADDRMAP_MAX_31 ?
			       DDR_ADDRMAP_UNUSED : map->rank[0] + RANK_B0_BASE;
	}

	if (priv->info.ranks > 2) {
		map->rank[1] = FIELD_GET(DDR_ADDRMAP_B8_M31, addrmap[0]);
		map->rank[1] = map->rank[1] == DDR_ADDRMAP_MAX_31 ?
			       DDR_ADDRMAP_UNUSED : map->rank[1] + RANK_B1_BASE;
	}
}

/**
 * snps_get_addr_map - Get HIF/SDRAM/etc address map from CSRs.
 * @priv:	DDR memory controller private instance data.
 *
 * Parse the controller registers content creating the addresses mapping tables.
 * They will be used for the erroneous and poison addresses encode/decode.
 */
static void snps_get_addr_map(struct snps_edac_priv *priv)
{
	u32 regval[DDR_ADDRMAP_NREGS];
	int i;

	for (i = 0; i < DDR_ADDRMAP_NREGS; i++)
		regval[i] = readl(priv->baseaddr + DDR_ADDRMAP0_OFST + i * 4);

	snps_get_hif_row_map(priv, regval);

	snps_get_hif_col_map(priv, regval);

	snps_get_hif_bank_map(priv, regval);

	snps_get_hif_bankgrp_map(priv, regval);

	snps_get_hif_rank_map(priv, regval);
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
	u32 size, row, width;
	int j;

	/* Actual SDRAM-word width for which ECC is calculated */
	width = 1U << (priv->info.dq_width - priv->info.dq_mode);

	for (row = 0; row < mci->nr_csrows; row++) {
		csi = mci->csrows[row];
		size = snps_get_memsize();

		for (j = 0; j < csi->nr_channels; j++) {
			dimm		= csi->channels[j]->dimm;
			dimm->edac_mode	= EDAC_SECDED;
			dimm->mtype	= priv->info.sdram_mode;
			dimm->nr_pages	= (size >> PAGE_SHIFT) / csi->nr_channels;
			dimm->grain	= width;
			dimm->dtype	= priv->info.dev_cfg;
		}
	}
}

/**
 * snps_mc_create - Create and initialize MC instance.
 * @priv:	DDR memory controller private data.
 *
 * Allocate the EDAC memory controller descriptor and initialize it
 * using the private data info.
 *
 * Return: MC data instance or negative errno.
 */
static struct mem_ctl_info *snps_mc_create(struct snps_edac_priv *priv)
{
	struct edac_mc_layer layers[2];
	struct mem_ctl_info *mci;

	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = SNPS_EDAC_NR_CSROWS;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = SNPS_EDAC_NR_CHANS;
	layers[1].is_virt_csrow = false;

	mci = edac_mc_alloc(EDAC_AUTO_MC_NUM, ARRAY_SIZE(layers), layers, 0);
	if (!mci) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed memory allocation for mc instance\n");
		return ERR_PTR(-ENOMEM);
	}

	mci->pvt_info = priv;
	mci->pdev = &priv->pdev->dev;
	platform_set_drvdata(priv->pdev, mci);

	/* Initialize controller capabilities and configuration */
	mci->mtype_cap = MEM_FLAG_LPDDR | MEM_FLAG_DDR2 | MEM_FLAG_LPDDR2 |
			 MEM_FLAG_DDR3 | MEM_FLAG_LPDDR3 |
			 MEM_FLAG_DDR4 | MEM_FLAG_LPDDR4;
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

	return mci;
}

/**
 * snps_mc_free - Free MC instance.
 * @mci:	EDAC memory controller instance.
 *
 * Just revert what was done in the framework of the snps_mc_create().
 *
 * Return: MC data instance or negative errno.
 */
static void snps_mc_free(struct mem_ctl_info *mci)
{
	struct snps_edac_priv *priv = mci->pvt_info;

	platform_set_drvdata(priv->pdev, NULL);

	edac_mc_free(mci);
}

static int snps_setup_irq(struct mem_ctl_info *mci)
{
	struct snps_edac_priv *priv = mci->pvt_info;
	int ret, irq;

	irq = platform_get_irq(priv->pdev, 0);
	if (irq < 0) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "No IRQ %d in DT\n", irq);
		return irq;
	}

	ret = devm_request_irq(&priv->pdev->dev, irq, snps_irq_handler,
			       0, dev_name(&priv->pdev->dev), mci);
	if (ret < 0) {
		edac_printk(KERN_ERR, EDAC_MC, "Failed to request IRQ\n");
		return ret;
	}

	snps_enable_irq(priv);

	return 0;
}

#ifdef CONFIG_EDAC_DEBUG

#define SNPS_DEBUGFS_FOPS(__name, __read, __write) \
	static const struct file_operations __name = {	\
		.owner = THIS_MODULE,		\
		.open = simple_open,		\
		.read = __read,			\
		.write = __write,		\
	}

#define SNPS_DBGFS_BUF_LEN 128

static int snps_ddrc_info_show(struct seq_file *s, void *data)
{
	struct mem_ctl_info *mci = s->private;
	struct snps_edac_priv *priv = mci->pvt_info;

	seq_printf(s, "SDRAM: %s\n", edac_mem_types[priv->info.sdram_mode]);

	seq_printf(s, "DQ bus: %u/%s\n", (BITS_PER_BYTE << priv->info.dq_width),
		   priv->info.dq_mode == SNPS_DQ_FULL ? "Full" :
		   priv->info.dq_mode == SNPS_DQ_HALF ? "Half" :
		   priv->info.dq_mode == SNPS_DQ_QRTR ? "Quarter" :
		   "Unknown");
	seq_printf(s, "Burst: SDRAM %u HIF %u\n", priv->info.sdram_burst_len,
		   priv->info.hif_burst_len);

	seq_printf(s, "Ranks: %u\n", priv->info.ranks);

	seq_printf(s, "ECC: %s\n",
		   priv->info.ecc_mode == SNPS_ECC_SECDED ? "SEC/DED" :
		   priv->info.ecc_mode == SNPS_ECC_ADVX4X8 ? "Advanced X4/X8" :
		   "Unknown");

	seq_puts(s, "Caps:");
	if (priv->info.caps) {
		if (priv->info.caps & SNPS_CAP_ZYNQMP)
			seq_puts(s, " +ZynqMP");
	} else {
		seq_puts(s, " -");
	}
	seq_putc(s, '\n');

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(snps_ddrc_info);

/**
 * snps_data_poison_setup - Update poison registers.
 * @priv:		DDR memory controller private instance data.
 *
 * Update poison registers as per DDR mapping.
 * Return: none.
 */
static void snps_data_poison_setup(struct snps_edac_priv *priv)
{
	struct snps_sdram_addr sdram;
	u32 regval;

	snps_map_sys_to_sdram(priv, priv->poison_addr, &sdram);

	regval = FIELD_PREP(ECC_POISON0_RANK_MASK, sdram.rank) |
		 FIELD_PREP(ECC_POISON0_COL_MASK, sdram.col);
	writel(regval, priv->baseaddr + ECC_POISON0_OFST);

	regval = FIELD_PREP(ECC_POISON1_BANKGRP_MASK, sdram.bankgrp) |
		 FIELD_PREP(ECC_POISON1_BANK_MASK, sdram.bank) |
		 FIELD_PREP(ECC_POISON1_ROW_MASK, sdram.row);
	writel(regval, priv->baseaddr + ECC_POISON1_OFST);
}

static ssize_t snps_inject_data_error_read(struct file *filep, char __user *ubuf,
					   size_t size, loff_t *offp)
{
	struct mem_ctl_info *mci = filep->private_data;
	struct snps_edac_priv *priv = mci->pvt_info;
	char buf[SNPS_DBGFS_BUF_LEN];
	int pos;

	pos = scnprintf(buf, sizeof(buf), "Poison0 Addr: 0x%08x\n\r",
			readl(priv->baseaddr + ECC_POISON0_OFST));
	pos += scnprintf(buf + pos, sizeof(buf) - pos, "Poison1 Addr: 0x%08x\n\r",
			 readl(priv->baseaddr + ECC_POISON1_OFST));
	pos += scnprintf(buf + pos, sizeof(buf) - pos, "Error injection Address: 0x%lx\n\r",
			 priv->poison_addr);

	return simple_read_from_buffer(ubuf, size, offp, buf, pos);
}

static ssize_t snps_inject_data_error_write(struct file *filep, const char __user *ubuf,
					    size_t size, loff_t *offp)
{
	struct mem_ctl_info *mci = filep->private_data;
	struct snps_edac_priv *priv = mci->pvt_info;
	int rc;

	rc = kstrtoul_from_user(ubuf, size, 0, &priv->poison_addr);
	if (rc)
		return rc;

	snps_data_poison_setup(priv);

	return size;
}

SNPS_DEBUGFS_FOPS(snps_inject_data_error, snps_inject_data_error_read,
		  snps_inject_data_error_write);

static ssize_t snps_inject_data_poison_read(struct file *filep, char __user *ubuf,
					    size_t size, loff_t *offp)
{
	struct mem_ctl_info *mci = filep->private_data;
	struct snps_edac_priv *priv = mci->pvt_info;
	char buf[SNPS_DBGFS_BUF_LEN];
	const char *errstr;
	u32 regval;
	int pos;

	regval = readl(priv->baseaddr + ECC_CFG1_OFST);
	errstr = FIELD_GET(ECC_CEPOISON_MASK, regval) == ECC_CEPOISON_MASK ?
		 "Correctable Error" : "UnCorrectable Error";

	pos = scnprintf(buf, sizeof(buf), "Data Poisoning: %s\n\r", errstr);

	return simple_read_from_buffer(ubuf, size, offp, buf, pos);
}

static ssize_t snps_inject_data_poison_write(struct file *filep, const char __user *ubuf,
					     size_t size, loff_t *offp)
{
	struct mem_ctl_info *mci = filep->private_data;
	struct snps_edac_priv *priv = mci->pvt_info;
	char buf[SNPS_DBGFS_BUF_LEN];
	int rc;

	rc = simple_write_to_buffer(buf, sizeof(buf), offp, ubuf, size);
	if (rc < 0)
		return rc;

	writel(0, priv->baseaddr + DDR_SWCTL);
	if (strncmp(buf, "CE", 2) == 0)
		writel(ECC_CEPOISON_MASK, priv->baseaddr + ECC_CFG1_OFST);
	else
		writel(ECC_UEPOISON_MASK, priv->baseaddr + ECC_CFG1_OFST);
	writel(1, priv->baseaddr + DDR_SWCTL);

	return size;
}

SNPS_DEBUGFS_FOPS(snps_inject_data_poison, snps_inject_data_poison_read,
		  snps_inject_data_poison_write);

/**
 * snps_create_debugfs_nodes -	Create DebugFS nodes.
 * @mci:	EDAC memory controller instance.
 *
 * Create DW uMCTL2 EDAC driver DebugFS nodes in the device private
 * DebugFS directory.
 *
 * Return: none.
 */
static void snps_create_debugfs_nodes(struct mem_ctl_info *mci)
{
	edac_debugfs_create_file("ddrc_info", 0400, mci->debugfs, mci,
				 &snps_ddrc_info_fops);

	edac_debugfs_create_file("inject_data_error", 0600, mci->debugfs, mci,
				 &snps_inject_data_error);

	edac_debugfs_create_file("inject_data_poison", 0600, mci->debugfs, mci,
				 &snps_inject_data_poison);
}

#else /* !CONFIG_EDAC_DEBUG */

static inline void snps_create_debugfs_nodes(struct mem_ctl_info *mci) {}

#endif /* !CONFIG_EDAC_DEBUG */

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
	struct snps_edac_priv *priv;
	struct mem_ctl_info *mci;
	int rc;

	priv = snps_create_data(pdev);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	rc = snps_get_ddrc_info(priv);
	if (rc)
		return rc;

	snps_get_addr_map(priv);

	mci = snps_mc_create(priv);
	if (IS_ERR(mci))
		return PTR_ERR(mci);

	rc = snps_setup_irq(mci);
	if (rc)
		goto free_edac_mc;

	rc = edac_mc_add_mc(mci);
	if (rc) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed to register with EDAC core\n");
		goto free_edac_mc;
	}

	snps_create_debugfs_nodes(mci);

	return 0;

free_edac_mc:
	snps_mc_free(mci);

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

	edac_mc_del_mc(&pdev->dev);

	snps_mc_free(mci);

	return 0;
}

static const struct of_device_id snps_edac_match[] = {
	{ .compatible = "xlnx,zynqmp-ddrc-2.40a", .data = zynqmp_init_plat },
	{ .compatible = "snps,ddrc-3.80a" },
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
