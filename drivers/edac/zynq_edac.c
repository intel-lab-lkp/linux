// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zynq DDR ECC Driver
 * This driver is based on ppc4xx_edac.c drivers
 *
 * Copyright (C) 2012 - 2014 Xilinx, Inc.
 */

#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "edac_module.h"

/* Number of cs_rows needed per memory controller */
#define ZYNQ_EDAC_NR_CSROWS		1

/* Number of channels per memory controller */
#define ZYNQ_EDAC_NR_CHANS		1

/* Granularity of reported error in bytes */
#define ZYNQ_EDAC_ERR_GRAIN		1

#define ZYNQ_EDAC_MSG_SIZE		256

#define ZYNQ_EDAC_MOD_STRING		"zynq_edac"
#define ZYNQ_EDAC_MOD_VER		"1"

/* Zynq DDR memory controller ECC registers */
#define ZYNQ_CTRL_OFST			0x0
#define ZYNQ_T_ZQ_OFST			0xA4

/* ECC control register */
#define ZYNQ_ECC_CTRL_OFST		0xC4
/* ECC log register */
#define ZYNQ_CE_LOG_OFST		0xC8
/* ECC address register */
#define ZYNQ_CE_ADDR_OFST		0xCC
/* ECC data[31:0] register */
#define ZYNQ_CE_DATA_31_0_OFST		0xD0

/* Uncorrectable error info registers */
#define ZYNQ_UE_LOG_OFST		0xDC
#define ZYNQ_UE_ADDR_OFST		0xE0
#define ZYNQ_UE_DATA_31_0_OFST		0xE4

#define ZYNQ_STAT_OFST			0xF0
#define ZYNQ_SCRUB_OFST			0xF4

/* Control register bit field definitions */
#define ZYNQ_CTRL_BW_MASK		0xC
#define ZYNQ_CTRL_BW_SHIFT		2

#define ZYNQ_DDRCTL_WDTH_16		1
#define ZYNQ_DDRCTL_WDTH_32		0

/* ZQ register bit field definitions */
#define ZYNQ_T_ZQ_DDRMODE_MASK		0x2

/* ECC control register bit field definitions */
#define ZYNQ_ECC_CTRL_CLR_CE_ERR	0x2
#define ZYNQ_ECC_CTRL_CLR_UE_ERR	0x1

/* ECC correctable/uncorrectable error log register definitions */
#define ZYNQ_LOG_VALID			0x1
#define ZYNQ_CE_LOG_BITPOS_MASK		0xFE
#define ZYNQ_CE_LOG_BITPOS_SHIFT	1

/* ECC correctable/uncorrectable error address register definitions */
#define ZYNQ_ADDR_COL_MASK		0xFFF
#define ZYNQ_ADDR_ROW_MASK		0xFFFF000
#define ZYNQ_ADDR_ROW_SHIFT		12
#define ZYNQ_ADDR_BANK_MASK		0x70000000
#define ZYNQ_ADDR_BANK_SHIFT		28

/* ECC statistic register definitions */
#define ZYNQ_STAT_UECNT_MASK		0xFF
#define ZYNQ_STAT_CECNT_MASK		0xFF00
#define ZYNQ_STAT_CECNT_SHIFT		8

/* ECC scrub register definitions */
#define ZYNQ_SCRUB_MODE_MASK		0x7
#define ZYNQ_SCRUB_MODE_SECDED		0x4

/**
 * struct zynq_ecc_error_info - ECC error log information.
 * @row:	Row number.
 * @col:	Column number.
 * @bank:	Bank number.
 * @bitpos:	Bit position.
 * @data:	Data causing the error.
 */
struct zynq_ecc_error_info {
	u32 row;
	u32 col;
	u32 bank;
	u32 bitpos;
	u32 data;
};

/**
 * struct zynq_ecc_status - ECC status information to report.
 * @ce_cnt:	Correctable error count.
 * @ue_cnt:	Uncorrectable error count.
 * @ceinfo:	Correctable error log information.
 * @ueinfo:	Uncorrectable error log information.
 */
struct zynq_ecc_status {
	u32 ce_cnt;
	u32 ue_cnt;
	struct zynq_ecc_error_info ceinfo;
	struct zynq_ecc_error_info ueinfo;
};

/**
 * struct zynq_edac_priv - DDR memory controller private instance data.
 * @baseaddr:	Base address of the DDR controller.
 * @message:	Buffer for framing the event specific info.
 * @stat:	ECC status information.
 */
struct zynq_edac_priv {
	void __iomem *baseaddr;
	char message[ZYNQ_EDAC_MSG_SIZE];
	struct zynq_ecc_status stat;
};

/**
 * zynq_get_error_info - Get the current ECC error info.
 * @priv:	DDR memory controller private instance data.
 *
 * Return: one if there is no error, otherwise zero.
 */
static int zynq_get_error_info(struct zynq_edac_priv *priv)
{
	struct zynq_ecc_status *p;
	u32 regval, clearval = 0;
	void __iomem *base;

	base = priv->baseaddr;
	p = &priv->stat;

	regval = readl(base + ZYNQ_STAT_OFST);
	if (!regval)
		return 1;

	p->ce_cnt = (regval & ZYNQ_STAT_CECNT_MASK) >> ZYNQ_STAT_CECNT_SHIFT;
	p->ue_cnt = regval & ZYNQ_STAT_UECNT_MASK;

	regval = readl(base + ZYNQ_CE_LOG_OFST);
	if (!(p->ce_cnt && (regval & ZYNQ_LOG_VALID)))
		goto ue_err;

	p->ceinfo.bitpos = (regval & ZYNQ_CE_LOG_BITPOS_MASK) >> ZYNQ_CE_LOG_BITPOS_SHIFT;
	regval = readl(base + ZYNQ_CE_ADDR_OFST);
	p->ceinfo.row = (regval & ZYNQ_ADDR_ROW_MASK) >> ZYNQ_ADDR_ROW_SHIFT;
	p->ceinfo.col = regval & ZYNQ_ADDR_COL_MASK;
	p->ceinfo.bank = (regval & ZYNQ_ADDR_BANK_MASK) >> ZYNQ_ADDR_BANK_SHIFT;
	p->ceinfo.data = readl(base + ZYNQ_CE_DATA_31_0_OFST);
	edac_dbg(3, "CE bit position: %d data: %d\n", p->ceinfo.bitpos,
		 p->ceinfo.data);
	clearval = ZYNQ_ECC_CTRL_CLR_CE_ERR;

ue_err:
	regval = readl(base + ZYNQ_UE_LOG_OFST);
	if (!(p->ue_cnt && (regval & ZYNQ_LOG_VALID)))
		goto out;

	regval = readl(base + ZYNQ_UE_ADDR_OFST);
	p->ueinfo.row = (regval & ZYNQ_ADDR_ROW_MASK) >> ZYNQ_ADDR_ROW_SHIFT;
	p->ueinfo.col = regval & ZYNQ_ADDR_COL_MASK;
	p->ueinfo.bank = (regval & ZYNQ_ADDR_BANK_MASK) >> ZYNQ_ADDR_BANK_SHIFT;
	p->ueinfo.data = readl(base + ZYNQ_UE_DATA_31_0_OFST);
	clearval |= ZYNQ_ECC_CTRL_CLR_UE_ERR;

out:
	writel(clearval, base + ZYNQ_ECC_CTRL_OFST);
	writel(0x0, base + ZYNQ_ECC_CTRL_OFST);

	return 0;
}

/**
 * zynq_handle_error - Handle Correctable and Uncorrectable errors.
 * @mci:	EDAC memory controller instance.
 * @p:		Zynq ECC status structure.
 *
 * Handles ECC correctable and uncorrectable errors.
 */
static void zynq_handle_error(struct mem_ctl_info *mci, struct zynq_ecc_status *p)
{
	struct zynq_edac_priv *priv = mci->pvt_info;
	struct zynq_ecc_error_info *pinf;

	if (p->ce_cnt) {
		pinf = &p->ceinfo;

		snprintf(priv->message, ZYNQ_EDAC_MSG_SIZE,
			 "Row %d Bank %d Col %d Bit %d Data 0x%08x",
			 pinf->row, pinf->bank, pinf->col,
			 pinf->bitpos, pinf->data);

		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci,
				     p->ce_cnt, 0, 0, 0, 0, 0, -1,
				     priv->message, "");
	}

	if (p->ue_cnt) {
		pinf = &p->ueinfo;

		snprintf(priv->message, ZYNQ_EDAC_MSG_SIZE,
			 "Row %d Bank %d Col %d",
			 pinf->row, pinf->bank, pinf->col);

		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci,
				     p->ue_cnt, 0, 0, 0, 0, 0, -1,
				     priv->message, "");
	}

	memset(p, 0, sizeof(*p));
}

/**
 * zynq_check_errors - Check controller for ECC errors.
 * @mci:	EDAC memory controller instance.
 *
 * Check and post ECC errors. Called by the polling thread.
 */
static void zynq_check_errors(struct mem_ctl_info *mci)
{
	struct zynq_edac_priv *priv = mci->pvt_info;
	int status;

	status = zynq_get_error_info(priv);
	if (status)
		return;

	zynq_handle_error(mci, &priv->stat);
}

/**
 * zynq_get_dtype - Return the controller memory width.
 * @base:	DDR memory controller base address.
 *
 * Get the EDAC device type width appropriate for the current controller
 * configuration.
 *
 * Return: a device type width enumeration.
 */
static enum dev_type zynq_get_dtype(const void __iomem *base)
{
	enum dev_type dt;
	u32 width;

	width = readl(base + ZYNQ_CTRL_OFST);
	width = (width & ZYNQ_CTRL_BW_MASK) >> ZYNQ_CTRL_BW_SHIFT;

	switch (width) {
	case ZYNQ_DDRCTL_WDTH_16:
		dt = DEV_X2;
		break;
	case ZYNQ_DDRCTL_WDTH_32:
		dt = DEV_X4;
		break;
	default:
		dt = DEV_UNKNOWN;
	}

	return dt;
}

/**
 * zynq_get_ecc_state - Return the controller ECC enable/disable status.
 * @base:	DDR memory controller base address.
 *
 * Get the ECC enable/disable status of the controller.
 *
 * Return: true if enabled, otherwise false.
 */
static bool zynq_get_ecc_state(void __iomem *base)
{
	enum dev_type dt;
	u32 ecctype;

	dt = zynq_get_dtype(base);
	if (dt == DEV_UNKNOWN)
		return false;

	ecctype = readl(base + ZYNQ_SCRUB_OFST) & ZYNQ_SCRUB_MODE_MASK;
	if ((ecctype == ZYNQ_SCRUB_MODE_SECDED) && (dt == DEV_X2))
		return true;

	return false;
}

/**
 * zynq_get_memsize - Read the size of the attached memory device.
 *
 * Return: the memory size in bytes.
 */
static u32 zynq_get_memsize(void)
{
	struct sysinfo inf;

	si_meminfo(&inf);

	return inf.totalram * inf.mem_unit;
}

/**
 * zynq_get_mtype - Return the controller memory type.
 * @base:	Zynq ECC status structure.
 *
 * Get the EDAC memory type appropriate for the current controller
 * configuration.
 *
 * Return: a memory type enumeration.
 */
static enum mem_type zynq_get_mtype(const void __iomem *base)
{
	enum mem_type mt;
	u32 memtype;

	memtype = readl(base + ZYNQ_T_ZQ_OFST);

	if (memtype & ZYNQ_T_ZQ_DDRMODE_MASK)
		mt = MEM_DDR3;
	else
		mt = MEM_DDR2;

	return mt;
}

/**
 * zynq_init_csrows - Initialize the csrow data.
 * @mci:	EDAC memory controller instance.
 *
 * Initialize the chip select rows associated with the EDAC memory
 * controller instance.
 */
static void zynq_init_csrows(struct mem_ctl_info *mci)
{
	struct zynq_edac_priv *priv = mci->pvt_info;
	struct csrow_info *csi;
	struct dimm_info *dimm;
	u32 size, row;
	int j;

	for (row = 0; row < mci->nr_csrows; row++) {
		csi = mci->csrows[row];
		size = zynq_get_memsize();

		for (j = 0; j < csi->nr_channels; j++) {
			dimm		= csi->channels[j]->dimm;
			dimm->edac_mode	= EDAC_SECDED;
			dimm->mtype	= zynq_get_mtype(priv->baseaddr);
			dimm->nr_pages	= (size >> PAGE_SHIFT) / csi->nr_channels;
			dimm->grain	= ZYNQ_EDAC_ERR_GRAIN;
			dimm->dtype	= zynq_get_dtype(priv->baseaddr);
		}
	}
}

/**
 * zynq_mc_init - Initialize one driver instance.
 * @mci:	EDAC memory controller instance.
 * @pdev:	platform device.
 *
 * Perform initialization of the EDAC memory controller instance and
 * related driver-private data associated with the memory controller the
 * instance is bound to.
 */
static void zynq_mc_init(struct mem_ctl_info *mci, struct platform_device *pdev)
{
	mci->pdev = &pdev->dev;
	platform_set_drvdata(pdev, mci);

	/* Initialize controller capabilities and configuration */
	mci->mtype_cap = MEM_FLAG_DDR3 | MEM_FLAG_DDR2;
	mci->edac_ctl_cap = EDAC_FLAG_NONE | EDAC_FLAG_SECDED;
	mci->scrub_cap = SCRUB_FLAG_HW_SRC;
	mci->scrub_mode = SCRUB_NONE;

	mci->edac_cap = EDAC_FLAG_SECDED;
	mci->ctl_name = "zynq_ddr_controller";
	mci->dev_name = ZYNQ_EDAC_MOD_STRING;
	mci->mod_name = ZYNQ_EDAC_MOD_VER;

	edac_op_state = EDAC_OPSTATE_POLL;
	mci->edac_check = zynq_check_errors;

	mci->ctl_page_to_phys = NULL;

	zynq_init_csrows(mci);
}

/**
 * zynq_mc_probe - Check controller and bind driver.
 * @pdev:	platform device.
 *
 * Probe a specific controller instance for binding with the driver.
 *
 * Return: 0 if the controller instance was successfully bound to the
 * driver; otherwise, < 0 on error.
 */
static int zynq_mc_probe(struct platform_device *pdev)
{
	struct edac_mc_layer layers[2];
	struct zynq_edac_priv *priv;
	struct mem_ctl_info *mci;
	void __iomem *baseaddr;
	int rc;

	baseaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(baseaddr))
		return PTR_ERR(baseaddr);

	if (!zynq_get_ecc_state(baseaddr)) {
		edac_printk(KERN_INFO, EDAC_MC, "ECC not enabled\n");
		return -ENXIO;
	}

	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = ZYNQ_EDAC_NR_CSROWS;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = ZYNQ_EDAC_NR_CHANS;
	layers[1].is_virt_csrow = false;

	mci = edac_mc_alloc(EDAC_AUTO_MC_NUM, ARRAY_SIZE(layers), layers,
			    sizeof(struct zynq_edac_priv));
	if (!mci) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed memory allocation for mc instance\n");
		return -ENOMEM;
	}

	priv = mci->pvt_info;
	priv->baseaddr = baseaddr;

	zynq_mc_init(mci, pdev);

	rc = edac_mc_add_mc(mci);
	if (rc) {
		edac_printk(KERN_ERR, EDAC_MC,
			    "Failed to register with EDAC core\n");
		goto free_edac_mc;
	}

	/*
	 * Start capturing the correctable and uncorrectable errors. A write of
	 * 0 starts the counters.
	 */
	writel(0x0, baseaddr + ZYNQ_ECC_CTRL_OFST);

	return 0;

free_edac_mc:
	edac_mc_free(mci);

	return rc;
}

/**
 * zynq_mc_remove - Unbind driver from controller.
 * @pdev:	Platform device.
 *
 * Return: Unconditionally 0
 */
static int zynq_mc_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci = platform_get_drvdata(pdev);

	edac_mc_del_mc(&pdev->dev);
	edac_mc_free(mci);

	return 0;
}

static const struct of_device_id zynq_edac_match[] = {
	{ .compatible = "xlnx,zynq-ddrc-a05" },
	{}
};
MODULE_DEVICE_TABLE(of, zynq_edac_match);

static struct platform_driver zynq_edac_mc_driver = {
	.driver = {
		   .name = "zynq-edac",
		   .of_match_table = zynq_edac_match,
		   },
	.probe = zynq_mc_probe,
	.remove = zynq_mc_remove,
};
module_platform_driver(zynq_edac_mc_driver);

MODULE_AUTHOR("Xilinx Inc");
MODULE_DESCRIPTION("Zynq DDR ECC driver");
MODULE_LICENSE("GPL v2");
