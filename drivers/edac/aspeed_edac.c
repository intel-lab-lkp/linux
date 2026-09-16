// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018, 2019 Cisco Systems
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/edac.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include "edac_module.h"

#define DRV_NAME "aspeed-edac"

#define ASPEED_MCR_PROT        0x00 /* protection key register */
#define ASPEED_MCR_CONF        0x04 /* configuration register */
#define ASPEED_MCR_INTR_CTRL   0x50 /* interrupt control/status register */
#define ASPEED_MCR_ADDR_UNREC  0x58 /* address of first un-recoverable error */
#define ASPEED_MCR_ADDR_REC    0x5c /* address of last recoverable error */

#define ASPEED_MCR_PROT_PASSWD          0xfc600309
#define ASPEED_MCR_CONF_DRAM_TYPE       BIT(4)
#define ASPEED_MCR_CONF_ECC             BIT(7)
#define ASPEED_MCR_INTR_CTRL_CLEAR      BIT(31)
#define ASPEED_MCR_INTR_CTRL_CNT_REC    GENMASK(23, 16)
#define ASPEED_MCR_INTR_CTRL_CNT_UNREC  GENMASK(15, 12)
#define ASPEED_MCR_INTR_CTRL_ENABLE     (BIT(0) | BIT(1))

struct aspeed_edac_chip {
	unsigned int conf_reg;
	u32 conf_ecc;
	u32 conf_dram_type;
	enum mem_type dram_type[2];
	unsigned long mtype_cap;
	unsigned int prot_reg;
	u32 prot_key;
};

struct aspeed_edac {
	raw_spinlock_t lock;

	void __iomem *regs __guarded_by(&lock);
	const struct aspeed_edac_chip *chip;
	int irq;
};

static void count_rec(struct mem_ctl_info *mci, u8 rec_cnt, u32 rec_addr)
{
	struct csrow_info *csrow = mci->csrows[0];
	u32 page, offset, syndrome;

	if (!rec_cnt)
		return;

	/* report first few errors (if there are) */
	/* note: no addresses are recorded */
	if (rec_cnt > 1) {
		/* page, offset and syndrome are not available */
		page = 0;
		offset = 0;
		syndrome = 0;
		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, rec_cnt-1,
				     page, offset, syndrome, 0, 0, -1,
				     "address(es) not available", "");
	}

	/* report last error */
	/* note: rec_addr is the last recoverable error addr */
	page = rec_addr >> PAGE_SHIFT;
	offset = rec_addr & ~PAGE_MASK;
	/* syndrome is not available */
	syndrome = 0;
	edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, 1,
			     csrow->first_page + page, offset, syndrome,
			     0, 0, -1, "", "");
}

static void count_un_rec(struct mem_ctl_info *mci, u8 un_rec_cnt,
			 u32 un_rec_addr)
{
	struct csrow_info *csrow = mci->csrows[0];
	u32 page, offset, syndrome;

	if (!un_rec_cnt)
		return;

	/* report 1. error */
	/* note: un_rec_addr is the first unrecoverable error addr */
	page = un_rec_addr >> PAGE_SHIFT;
	offset = un_rec_addr & ~PAGE_MASK;
	/* syndrome is not available */
	syndrome = 0;
	edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci, 1,
			     csrow->first_page + page, offset, syndrome,
			     0, 0, -1, "", "");

	/* report further errors (if there are) */
	/* note: no addresses are recorded */
	if (un_rec_cnt > 1) {
		/* page, offset and syndrome are not available */
		page = 0;
		offset = 0;
		syndrome = 0;
		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci, un_rec_cnt-1,
				     page, offset, syndrome, 0, 0, -1,
				     "address(es) not available", "");
	}
}

static void aspeed_mcr_irq_update_enter(struct aspeed_edac *priv)
	__must_hold(&priv->lock)
{
	if (priv->chip->prot_key)
		writel(priv->chip->prot_key, priv->regs + priv->chip->prot_reg);
}

static void aspeed_mcr_irq_update_exit(struct aspeed_edac *priv)
	__must_hold(&priv->lock)
{
	if (priv->chip->prot_key)
		writel(~priv->chip->prot_key, priv->regs + priv->chip->prot_reg);
}

static irqreturn_t aspeed_mcr_isr(int irq, void *arg)
{
	struct mem_ctl_info *mci = arg;
	u32 rec_addr, un_rec_addr;
	struct aspeed_edac *priv;
	u8 rec_cnt, un_rec_cnt;
	u32 reg50;

	priv = mci->pvt_info;

	scoped_guard(raw_spinlock, &priv->lock) {
		reg50 = readl(priv->regs + ASPEED_MCR_INTR_CTRL);
		un_rec_addr = readl(priv->regs + ASPEED_MCR_ADDR_UNREC);
		rec_addr = readl(priv->regs + ASPEED_MCR_ADDR_REC);

		/*
		 * Clearing the counters needs a set-then-clear of CLEAR. The
		 * counter and interrupt flag fields are read-only, so writing
		 * back the values read above leaves them unaffected.
		 */
		aspeed_mcr_irq_update_enter(priv);
		writel(reg50 | ASPEED_MCR_INTR_CTRL_CLEAR,
		       priv->regs + ASPEED_MCR_INTR_CTRL);
		writel(reg50 & ~ASPEED_MCR_INTR_CTRL_CLEAR,
		       priv->regs + ASPEED_MCR_INTR_CTRL);
		aspeed_mcr_irq_update_exit(priv);
	}

	dev_dbg(mci->pdev, "received edac interrupt w/ mcr register 50: 0x%x\n",
		reg50);

	/* collect data about recoverable and unrecoverable errors */
	rec_cnt = FIELD_GET(ASPEED_MCR_INTR_CTRL_CNT_REC, reg50);
	un_rec_cnt = FIELD_GET(ASPEED_MCR_INTR_CTRL_CNT_UNREC, reg50);

	dev_dbg(mci->pdev, "%d recoverable interrupts and %d unrecoverable interrupts\n",
		rec_cnt, un_rec_cnt);

	/* process recoverable and unrecoverable errors */
	count_rec(mci, rec_cnt, rec_addr);
	count_un_rec(mci, un_rec_cnt, un_rec_addr);

	if (!rec_cnt && !un_rec_cnt)
		dev_dbg_ratelimited(mci->pdev, "received edac interrupt, but did not find any ECC counters\n");

	scoped_guard(raw_spinlock, &priv->lock)
		reg50 = readl(priv->regs + ASPEED_MCR_INTR_CTRL);
	dev_dbg(mci->pdev, "edac interrupt handled. mcr reg 50 is now: 0x%x\n",
		reg50);

	return IRQ_HANDLED;
}

static void aspeed_set_irq(struct aspeed_edac *priv, bool enable)
{
	u32 val;

	guard(raw_spinlock_irqsave)(&priv->lock);

	val = readl(priv->regs + ASPEED_MCR_INTR_CTRL);
	if (enable)
		val |= ASPEED_MCR_INTR_CTRL_ENABLE;
	else
		val &= ~ASPEED_MCR_INTR_CTRL_ENABLE;

	aspeed_mcr_irq_update_enter(priv);
	writel(val, priv->regs + ASPEED_MCR_INTR_CTRL);
	aspeed_mcr_irq_update_exit(priv);
}

static int config_irq(struct mem_ctl_info *mci, struct platform_device *pdev)
{
	struct aspeed_edac *priv = mci->pvt_info;
	int irq;
	int rc;

	/* register interrupt handler */
	irq = platform_get_irq(pdev, 0);
	dev_dbg(&pdev->dev, "got irq %d\n", irq);
	if (irq < 0)
		return irq;

	rc = devm_request_irq(&pdev->dev, irq, aspeed_mcr_isr, IRQF_TRIGGER_HIGH,
			      DRV_NAME, mci);
	if (rc)
		return rc;

	priv->irq = irq;

	/* enable interrupts */
	aspeed_set_irq(priv, true);

	return 0;
}

static int init_csrows(struct mem_ctl_info *mci)
{
	struct csrow_info *csrow = mci->csrows[0];
	struct aspeed_edac *priv = mci->pvt_info;
	struct device_node *np;
	struct dimm_info *dimm;
	struct resource r;
	unsigned int type;
	u32 nr_pages;
	u32 conf;
	int rc;

	/* retrieve info about physical memory from device tree */
	np = of_find_node_by_name(NULL, "memory");
	if (!np) {
		dev_err(mci->pdev, "dt: missing /memory node\n");
		return -ENODEV;
	}

	rc = of_address_to_resource(np, 0, &r);

	of_node_put(np);

	if (rc) {
		dev_err(mci->pdev, "dt: failed requesting resource for /memory node\n");
		return rc;
	}

	dev_dbg(mci->pdev, "dt: /memory node resources: first page %pR, PAGE_SHIFT macro=0x%x\n",
		&r, PAGE_SHIFT);

	csrow->first_page = r.start >> PAGE_SHIFT;
	nr_pages = resource_size(&r) >> PAGE_SHIFT;
	csrow->last_page = csrow->first_page + nr_pages - 1;

	scoped_guard(raw_spinlock_irqsave, &priv->lock)
		conf = readl(priv->regs + priv->chip->conf_reg);
	type = field_get(priv->chip->conf_dram_type, conf);

	dimm = csrow->channels[0]->dimm;
	dimm->mtype = priv->chip->dram_type[type];
	dimm->edac_mode = EDAC_SECDED;
	dimm->nr_pages = nr_pages / csrow->nr_channels;
	dimm->grain = 16;

	dev_dbg(mci->pdev, "initialized dimm with first_page=0x%lx and nr_pages=0x%x\n",
		csrow->first_page, nr_pages);

	return 0;
}

static int aspeed_probe(struct platform_device *pdev)
{
	const struct aspeed_edac_chip *chip;
	struct device *dev = &pdev->dev;
	struct edac_mc_layer layers[2];
	struct aspeed_edac *priv;
	struct mem_ctl_info *mci;
	void __iomem *regs;
	u32 conf;
	int rc;

	chip = of_device_get_match_data(dev);
	if (!chip)
		return -EINVAL;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	/* bail out if ECC mode is not configured */
	conf = readl(regs + chip->conf_reg);
	if (!field_get(chip->conf_ecc, conf)) {
		dev_err(&pdev->dev, "ECC mode is not configured in u-boot\n");
		return -EPERM;
	}

	edac_op_state = EDAC_OPSTATE_INT;

	/* allocate & init EDAC MC data structure */
	layers[0].type = EDAC_MC_LAYER_CHIP_SELECT;
	layers[0].size = 1;
	layers[0].is_virt_csrow = true;
	layers[1].type = EDAC_MC_LAYER_CHANNEL;
	layers[1].size = 1;
	layers[1].is_virt_csrow = false;

	mci = edac_mc_alloc(0, ARRAY_SIZE(layers), layers, sizeof(*priv));
	if (!mci)
		return -ENOMEM;

	priv = mci->pvt_info;
	priv->chip = chip;
	scoped_guard(raw_spinlock_init, &priv->lock)
		priv->regs = regs;

	mci->pdev = &pdev->dev;
	mci->mtype_cap = chip->mtype_cap;
	mci->edac_ctl_cap = EDAC_FLAG_SECDED;
	mci->edac_cap = EDAC_FLAG_SECDED;
	mci->scrub_cap = SCRUB_FLAG_HW_SRC;
	mci->scrub_mode = SCRUB_HW_SRC;
	mci->mod_name = DRV_NAME;
	mci->ctl_name = "MIC";
	mci->dev_name = dev_name(&pdev->dev);

	rc = init_csrows(mci);
	if (rc) {
		dev_err(&pdev->dev, "failed to init csrows\n");
		goto probe_exit02;
	}

	platform_set_drvdata(pdev, mci);

	/* register with edac core */
	rc = edac_mc_add_mc(mci);
	if (rc) {
		dev_err(&pdev->dev, "failed to register with EDAC core\n");
		goto probe_exit02;
	}

	/* register interrupt handler and enable interrupts */
	rc = config_irq(mci, pdev);
	if (rc) {
		dev_err(&pdev->dev, "failed setting up irq\n");
		goto probe_exit01;
	}

	return 0;

probe_exit01:
	edac_mc_del_mc(&pdev->dev);
probe_exit02:
	edac_mc_free(mci);
	return rc;
}

static void aspeed_remove(struct platform_device *pdev)
{
	struct mem_ctl_info *mci = platform_get_drvdata(pdev);
	struct aspeed_edac *priv = mci->pvt_info;

	/* disable interrupts */
	aspeed_set_irq(priv, false);

	devm_free_irq(&pdev->dev, priv->irq, mci);

	/* free resources */
	edac_mc_del_mc(&pdev->dev);
	edac_mc_free(mci);
}

static const struct aspeed_edac_chip ast2400_edac = {
	.conf_reg = ASPEED_MCR_CONF,
	.conf_ecc = ASPEED_MCR_CONF_ECC,
	.conf_dram_type = ASPEED_MCR_CONF_DRAM_TYPE,
	.dram_type = { MEM_DDR3, MEM_DDR4 },
	.mtype_cap = MEM_FLAG_DDR3 | MEM_FLAG_DDR4,
	.prot_reg = ASPEED_MCR_PROT,
	.prot_key = ASPEED_MCR_PROT_PASSWD,
};

/* The AST2600 does not key-protect the interrupt control register (MCR50). */
static const struct aspeed_edac_chip ast2600_edac = {
	.conf_reg = ASPEED_MCR_CONF,
	.conf_ecc = ASPEED_MCR_CONF_ECC,
	.conf_dram_type = ASPEED_MCR_CONF_DRAM_TYPE,
	.dram_type = { MEM_DDR3, MEM_DDR4 },
	.mtype_cap = MEM_FLAG_DDR3 | MEM_FLAG_DDR4,
};

static const struct of_device_id aspeed_of_match[] = {
	{ .compatible = "aspeed,ast2400-sdram-edac", .data = &ast2400_edac },
	{ .compatible = "aspeed,ast2500-sdram-edac", .data = &ast2400_edac },
	{ .compatible = "aspeed,ast2600-sdram-edac", .data = &ast2600_edac },
	{},
};

MODULE_DEVICE_TABLE(of, aspeed_of_match);

static struct platform_driver aspeed_driver = {
	.driver		= {
		.name	= DRV_NAME,
		.of_match_table = aspeed_of_match
	},
	.probe		= aspeed_probe,
	.remove		= aspeed_remove
};
module_platform_driver(aspeed_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stefan Schaeckeler <sschaeck@cisco.com>");
MODULE_DESCRIPTION("Aspeed BMC SoC EDAC driver");
MODULE_VERSION("1.0");
