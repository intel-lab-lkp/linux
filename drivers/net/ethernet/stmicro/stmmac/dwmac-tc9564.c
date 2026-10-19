// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 *
 * Derived from code having the following copyrights:
 * Copyright (C) 2011-2012  Vayavya Labs Pvt Ltd
 * Copyright (C) 2025 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/pcs/pcs-xpcs-regmap.h>
#include <linux/pcs/pcs-xpcs.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/stmmac.h>
#include <linux/types.h>
#include <linux/units.h>

#include "common.h"
#include "dwxgmac2.h"
#include "stmmac.h"
#include "stmmac_platform.h"

#define DRIVER_NAME			"dwmac-tc9564"

/* Start of range representing PCI space on the TC9564 internal bus */
#define TC9564_SLV00_SRC_ADDR		0x0000001000000000ULL

#define TC9564_PTP_CLOCK_RATE		(250 * HZ_PER_MHZ)

#define TC9564_RX_FIFO_SIZE		(46 * SZ_1K)
#define TC9564_TX_FIFO_SIZE		(46 * SZ_1K)

/* Fields and values for the EMACTL registers */
#define EMAC_SP_SEL_MASK		GENMASK(3, 0)
#define SP_SEL_2500BASEX		4
#define SP_SEL_SGMII_1000M		5
#define SP_SEL_SGMII_100M		6
#define SP_SEL_SGMII_10M		7
#define EMAC_PHY_INF_SEL_MASK		GENMASK(5, 4)
#define PCS_CLK_PHY			1	/* Clock from PHY */
#define EMAC_INV_SGM_SIG_DET		BIT(6)	/* 1 = polarity inverted */
#define EMAC_LPIHWCLKEN			BIT(8)	/* 1 = low power mode */
#define EMAC_INIT_DONE			BIT(21)

/* Offset to the XPCS memory block, relative to the EMAC address range */
#define DWMAC_XPCS_OFFSET		0x3a00

/* Offset to the PMATOP memory block, relative to the EMAC address range */
#define DWMAC_PMATOP_OFFSET			0x4000

#define PMA_CML_GL_PM_CFG0			0x01b8

/*
 * Five sets three registers must be configured for PMA.  The HWT_REFCLK
 * registers are each separated by 0x14 bytes.  The Common0 configuration
 * registers are separated by 0x8 bytes.
 */
#define PMA_REG_COUNT				5

#define PMA_HWT_REFCK_R_EN			0x1080
#define PMA_HWT_REFCK_TERM_EN			0x1090
#define PMA_HWT_REFCK_STRIDE			0x0014

#define PMA_COMM_CFG_0_1			0x1888
#define PMA_COMM_CFG_0_1_STRIDE			0x0008

/* PMA_COMM_CFG_0_1 fields (WRITE_MASK is a field name) */
#define COMM_CFG_WRITE_MASK_MASK		GENMASK(16, 9)
#define WRITE_MASK_VALUE			0xf7	/* Power-on value */
#define COMM_CFG_ENABLE				BIT(8)
#define COMM_CFG_WRITE_DATA_MASK		GENMASK(7, 0)
#define WRITE_DATA_VALUE			0x04	/* Power-on value */

enum {
	RESET_ID_MAC,
	RESET_ID_XPCS,
	RESET_ID_PMA,
};

static const char * const tc9564_reset_names[] = {
	[RESET_ID_MAC] = "mac",
	[RESET_ID_XPCS] = "xpcs",
	[RESET_ID_PMA] = "pma",
};

/**
 * struct tc9564_data - Toshiba-specific platform data
 * @dev:		Device pointer
 * @ioaddr:		Pointer to mapped eMAC memory
 * @plat:		Pointer to our stmmac platform data
 * @resets:		Reset controller bulk array
 * @clocks:		Clock controller bulk array
 * @clock_count:	Number of valid elements in the clock array
 * @config_regmap:	Regmap used to access eMAC configuration registers
 * @emac_ctl_offset:	Offset of the eMAC control register within the regmap
 * @res:		Structure passed to stmmac_dvr_probe()
 * @desc:		DMA descriptor data used by mac_device_info
 * @dma:		DMA operations data used by mac_device_info
 */
struct tc9564_data {
	struct device *dev;
	void __iomem *ioaddr;
	struct plat_stmmacenet_data *plat;
	struct reset_control_bulk_data resets[ARRAY_SIZE(tc9564_reset_names)];
	struct clk_bulk_data *clocks;
	u32 clock_count;

	struct regmap *config_regmap;
	u32 emac_ctl_offset;

	/* This field is data passed to stmmac_dvr_probe() */
	struct stmmac_resources res;

	/* These two fields are used by the mac_device_info structure */
	struct stmmac_desc_ops desc;
	struct stmmac_dma_ops dma;
};

struct tc9564_mac_speed {
	phy_interface_t phy_interface;
	int speed;
	u32 sp_sel;
};

static struct tc9564_mac_speed mac_speed[] = {
	{ PHY_INTERFACE_MODE_2500BASEX,	SPEED_2500,  SP_SEL_2500BASEX },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_1000,  SP_SEL_SGMII_1000M },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_100,   SP_SEL_SGMII_100M },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_10,    SP_SEL_SGMII_10M },
};

/* TC9564 uses indirect addressing so this need only describe a 1KiB range */
static const struct regmap_config xpcs_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_base	= 0x00,		/* Minimum XPCS reg offset */
	.max_register	= 0xff,		/* Register DW_VR_CSR_VIEWPORT */
	.reg_shift	= REGMAP_UPSHIFT(2),
};

/**
 * tc9564_pma_init() - Initialize PMA
 * @td:	bsp_priv pointer
 *
 * Initialize (or re-initialize) the PMA, configure the clocks and wait for the
 * eMAC to be ready.
 */
static void tc9564_pma_init(struct tc9564_data *td)
{
	struct reset_control *rstc = td->resets[RESET_ID_PMA].rstc;
	struct regmap *regmap = td->config_regmap;
	void __iomem *pmatop;
	u32 val;
	int ret;
	u32 i;

	/*
	 * When we re-initialize the PMA then the reset will already have
	 * been deasserted. We must make sure the PMA reset is asserted
	 * before we change the clock settings.
	 */
	WARN_ON(reset_control_assert(rstc));

	pmatop = td->res.addr + DWMAC_PMATOP_OFFSET;

	/* Power on CML buffer (0 = normal mode, 1 = power down) */
	writel(0, pmatop + PMA_CML_GL_PM_CFG0);

	/* This value switches clock from C0_REFCK to CLK_REF_I */
	val = u32_encode_bits(WRITE_MASK_VALUE, COMM_CFG_WRITE_MASK_MASK);
	val |= COMM_CFG_ENABLE;
	val |= u32_encode_bits(WRITE_DATA_VALUE, COMM_CFG_WRITE_DATA_MASK);

	for (i = 0; i < PMA_REG_COUNT; i++) {
		u32 offset =  i * PMA_HWT_REFCK_STRIDE;

		/* Disable C0_REFCK and 100 ohm termination */
		writel(0, pmatop + PMA_HWT_REFCK_R_EN + offset);
		writel(0, pmatop + PMA_HWT_REFCK_TERM_EN + offset);

		/* Switch clock from C0_REFCK to CLK_REF_I */
		offset =  i * PMA_COMM_CFG_0_1_STRIDE;
		writel(val, pmatop + PMA_COMM_CFG_0_1 + offset);
	}

	WARN_ON(reset_control_deassert(rstc));

	ret = regmap_read_poll_timeout(regmap, td->emac_ctl_offset,
				       val, val & EMAC_INIT_DONE, 50, 1000000);
	WARN(ret, "timeout waiting for MAC init done\n");
}

static int tc9564_mac_speed_select(struct tc9564_data *td,
				   phy_interface_t interface, int speed)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mac_speed); i++) {
		if (mac_speed[i].phy_interface != interface)
			continue;

		if (speed == mac_speed[i].speed || speed == SPEED_UNKNOWN)
			return mac_speed[i].sp_sel;
	}
	dev_err(td->dev, "%s/%d unsupported\n", phy_modes(interface), speed);

	return -EOPNOTSUPP;
}

static void tc9564_mac_configure(struct tc9564_data *td,
				 phy_interface_t interface, int speed)
{
	struct regmap *regmap = td->config_regmap;
	u32 offset = td->emac_ctl_offset;
	int sp_sel;
	u32 val;

	sp_sel = tc9564_mac_speed_select(td, interface, speed);
	if (sp_sel < 0)
		return;

	/* No error checking needed for MMIO regmap */
	regmap_read(regmap, offset, &val);
	val |= EMAC_LPIHWCLKEN;
	val &= ~EMAC_INV_SGM_SIG_DET;
	val = u32_replace_bits(val, PCS_CLK_PHY, EMAC_PHY_INF_SEL_MASK);
	val = u32_replace_bits(val, sp_sel, EMAC_SP_SEL_MASK);
	regmap_write(regmap, offset, val);
}

static void tc9564_mac_enable(struct tc9564_data *td)
{
	WARN_ON(clk_bulk_prepare_enable(td->clock_count, td->clocks));

	WARN_ON(reset_control_deassert(td->resets[RESET_ID_MAC].rstc));

	tc9564_mac_configure(td, td->plat->phy_interface, SPEED_UNKNOWN);
	tc9564_pma_init(td);

	WARN_ON(reset_control_deassert(td->resets[RESET_ID_XPCS].rstc));
}

static void tc9564_mac_disable(struct tc9564_data *td)
{
	WARN_ON(reset_control_bulk_assert(ARRAY_SIZE(td->resets), td->resets));

	clk_bulk_disable_unprepare(td->clock_count, td->clocks);
}

/*
 * Override method for dwxgmac301_dma_ops->init_rx_chan
 *
 * This differs from the dwxgmac301_dma_ops->init_rx_chan by translating the DMA
 * address for TC9564 internal bus. The window that provides DMA access to PCI
 * is linearly mapped at 0x10_0000_0000.
 */
static void tc9564_dma_init_rx_chan(struct stmmac_priv *priv,
				    void __iomem *ioaddr,
				    struct stmmac_dma_cfg *dma_cfg,
				    dma_addr_t phy, u32 chan)
{
	u64 translated = phy + TC9564_SLV00_SRC_ADDR;

	dwxgmac2_dma_init_rx_chan(priv, ioaddr, dma_cfg, phy, chan);

	writel(upper_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}

/* Override method for dwxgmac301_dma_ops->init_tx_chan */
static void tc9564_dma_init_tx_chan(struct stmmac_priv *priv,
				    void __iomem *ioaddr,
				    struct stmmac_dma_cfg *dma_cfg,
				    dma_addr_t phy, u32 chan)
{
	u64 translated = phy + TC9564_SLV00_SRC_ADDR;

	dwxgmac2_dma_init_tx_chan(priv, ioaddr, dma_cfg, phy, chan);

	writel(upper_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

/* Override method for dwxgmac210_desc_ops->set_addr */
static void tc9564_desc_set_addr(struct dma_desc *p, dma_addr_t addr)
{
	u64 translated = addr + TC9564_SLV00_SRC_ADDR;

	p->des0 = cpu_to_le32(lower_32_bits(translated));
	p->des1 = cpu_to_le32(upper_32_bits(translated));
}

/* Override method for dwxgmac210_desc_ops->set_sec_addr */
static void tc9564_desc_set_sec_addr(struct dma_desc *p, dma_addr_t addr,
				     bool is_valid)
{
	u64 translated = addr + TC9564_SLV00_SRC_ADDR;

	p->des2 = cpu_to_le32(lower_32_bits(translated));
	p->des3 = cpu_to_le32(upper_32_bits(translated));
}

/*
 * Use mac_setup to apply the override methods above.
 *
 * The memory for the modified ops structures is pre-allocated as part of
 * struct tc9564_data.
 */
static int tc9564_mac_setup(void *apriv, struct mac_device_info *mac)
{
	struct stmmac_priv *priv = apriv;
	struct tc9564_data *td;

	td = priv->plat->bsp_priv;

	/* dwxgmac301_dma_ops needs extending to provide DMA address translation */
	td->dma = dwxgmac301_dma_ops;
	td->dma.init_rx_chan = tc9564_dma_init_rx_chan;
	td->dma.init_tx_chan = tc9564_dma_init_tx_chan;
	mac->dma = &td->dma;

	/* dwxgmac210_desc_ops also needs extending for the same reason */
	td->desc = dwxgmac210_desc_ops;
	td->desc.set_addr = tc9564_desc_set_addr;
	td->desc.set_sec_addr = tc9564_desc_set_sec_addr;
	mac->desc = &td->desc;

	priv->hw = mac;

	return dwxgmac2_setup(priv);
}

static int tc9564_pcs_init(struct stmmac_priv *priv)
{
	void __iomem *emac = priv->ioaddr;
	struct regmap *xpcs_regmap;
	void __iomem *xpcs_addr;
	struct dw_xpcs *xpcs;

	xpcs_addr = emac + DWMAC_XPCS_OFFSET;
	xpcs_regmap = devm_regmap_init_mmio(priv->device, xpcs_addr,
					    &xpcs_regmap_config);
	if (IS_ERR(xpcs_regmap))
		return PTR_ERR(xpcs_regmap);

	xpcs = devm_xpcs_regmap_register(priv->device, xpcs_regmap);
	if (IS_ERR(xpcs))
		return PTR_ERR(xpcs);

	xpcs_config_eee_mult_fact(xpcs, priv->plat->mult_fact_100ns);
	priv->hw->phylink_pcs = xpcs_to_phylink_pcs(xpcs);

	return 0;
}

static struct phylink_pcs *tc9564_select_pcs(struct stmmac_priv *priv,
					     phy_interface_t interface)
{
	return priv->hw->phylink_pcs;
}

static void tc9564_fix_mac_speed(void *bsp_priv, phy_interface_t interface,
				 int speed, unsigned int mode)
{
	struct tc9564_data *td = bsp_priv;

	tc9564_mac_configure(td, interface, speed);
	tc9564_pma_init(td);
}

static int tc9564_dwmac_suspend(struct device *dev, void *bsp_priv)
{
	struct tc9564_data *td = bsp_priv;

	tc9564_mac_disable(td);

	return 0;
}

static int tc9564_dwmac_resume(struct device *dev, void *bsp_priv)
{
	struct tc9564_data *td = bsp_priv;

	tc9564_mac_enable(td);

	return 0;
}

static int tc9564_probe_config_dt(struct tc9564_data *td)
{
	struct platform_device *pdev = to_platform_device(td->dev);
	struct device_node *np = td->dev->of_node;
	struct plat_stmmacenet_data *plat;
	phy_interface_t phy_interface;
	struct device *dev = td->dev;
	int ret;

	ret = device_get_phy_mode(dev);
	if (ret < 0)
		return -ENODEV;
	phy_interface = ret;

	/* The platform structure is allocated with devm_kzalloc() */
	plat = devm_stmmac_probe_config_dt(pdev, td->res.mac);
	if (IS_ERR(plat))
		return PTR_ERR(plat);

	/*
	 * Core driver cannot deduce or validate fifo size because the bits
	 * that provide FIFO size only worth in powers of two.
	 */
	if (plat->rx_fifo_size == 0 || plat->tx_fifo_size == 0)
		return dev_err_probe(dev, -EINVAL, "fifo size is not set\n");
	if (plat->rx_fifo_size > TC9564_RX_FIFO_SIZE ||
	    plat->tx_fifo_size > TC9564_TX_FIFO_SIZE)
		return dev_err_probe(dev, -EINVAL, "fifo size too big (%u/%u)\n",
				     plat->rx_fifo_size, plat->tx_fifo_size);

	/*
	 * toshiba,tc9564-xgmac is *not* compatible with snps,dwxgmac because
	 * generic drivers cannot possibly work on TC9564 (without clocks
	 * reading registers wedges the bus). However, without snps,dwxgmac,
	 * devm_stmmac_probe_config_dt(), can't figure of the core type and
	 * won't parse a couple of extra properties. Let's handle that here!
	 */
	plat->core_type = DWMAC_CORE_XGMAC;
	plat->pmt = true;
	if (of_property_read_bool(np, "snps,tso"))
		plat->flags |= STMMAC_FLAG_TSO_EN;
	of_property_read_u32(np, "snps,multicast-filter-bins",
			     &plat->multicast_filter_bins);
	of_property_read_u32(np, "snps,perfect-filter-entries",
			     &plat->unicast_filter_entries);

	plat->phy_interface = phy_interface;

	/*
	 * Set a few additional flags and settings that are always needed for
	 * TC9564.
	 */
	plat->default_an_inband = true;
	plat->host_dma_width = 36;
	plat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	for (int i = 4; i < plat->tx_queues_to_use; i++) {
		plat->tx_queues_cfg[i].coe_unsupported = true;
		plat->tx_queues_cfg[i].tbs_en = 1;
	}

	plat->fix_mac_speed = tc9564_fix_mac_speed;
	plat->suspend = tc9564_dwmac_suspend;
	plat->resume = tc9564_dwmac_resume;
	plat->mac_setup = tc9564_mac_setup;
	plat->pcs_init = tc9564_pcs_init;
	plat->select_pcs = tc9564_select_pcs;

	plat->bsp_priv = td;

	td->plat = plat;

	return 0;
}

/* Look up resets, and ensure they're initially all asserted */
static int tc9564_reset_init(struct tc9564_data *td)
{
	u32 reset_count = ARRAY_SIZE(td->resets);
	int ret;
	u32 i;

	for (i = 0; i < reset_count; i++)
		td->resets[i].id = tc9564_reset_names[i];

	ret = devm_reset_control_bulk_get_exclusive(td->dev, reset_count,
						    td->resets);
	if (ret)
		return ret;

	return reset_control_bulk_assert(reset_count, td->resets);
}

static int tc9564_clock_init(struct tc9564_data *td)
{
	struct device *dev = td->dev;
	int ret;

	ret = devm_clk_bulk_get_all(dev, &td->clocks);
	if (ret < 0)
		return ret;
	td->clock_count = ret;

	return 0;
}

static int tc9564_dwmac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tc9564_data *td;
	struct device_node *np;
	struct regmap *regmap;
	int ret;

	td = devm_kzalloc(dev, sizeof(*td), GFP_KERNEL);
	if (!td)
		return -ENOMEM;

	td->dev = dev;

	np = dev_of_node(dev);
	regmap = syscon_regmap_lookup_by_phandle_args(np,
						      "toshiba,config-syscon",
						      1, &td->emac_ctl_offset);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get config regmap\n");
	td->config_regmap = regmap;

	ret = tc9564_reset_init(td);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize resets\n");

	ret = tc9564_clock_init(td);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize clocks\n");

	ret = tc9564_probe_config_dt(td);
	if (ret)
		return ret;

	ret = stmmac_get_platform_resources(pdev, &td->res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize stmmac resources\n");

	tc9564_mac_enable(td);

	ret = stmmac_dvr_probe(dev, td->plat, &td->res);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed stmmac probe\n");
		goto err_disable_mac;
	}

	return 0;

err_disable_mac:
	tc9564_mac_disable(td);

	return ret;
}

static void tc9564_dwmac_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct tc9564_data *td = priv->plat->bsp_priv;

	stmmac_dvr_remove(dev);
	tc9564_mac_disable(td);
}

static const struct of_device_id tc9564_dwmac_ids[] = {
	{ .compatible = "toshiba,tc9564-xgmac", },
	{ },
};
MODULE_DEVICE_TABLE(of, tc9564_dwmac_ids);

static struct platform_driver tc9564_dwmac_driver = {
	.probe			= tc9564_dwmac_probe,
	.remove			= tc9564_dwmac_remove,
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= tc9564_dwmac_ids,
		.pm		= &stmmac_simple_pm_ops,
		.owner		= THIS_MODULE,
	},
};
module_platform_driver(tc9564_dwmac_driver);

MODULE_DESCRIPTION("Toshiba TC9564 PCIe Ethernet Network Driver");
MODULE_LICENSE("GPL");
