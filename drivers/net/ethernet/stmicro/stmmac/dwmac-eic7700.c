// SPDX-License-Identifier: GPL-2.0
/*
 * Eswin DWC Ethernet linux driver
 *
 * Authors: Shuang Liang <liangshuang@eswincomputing.com>
 * Shangjuan Wei <weishangjuan@eswincomputing.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"
#include "dwmac4.h"

#include <linux/regmap.h>

/* eth_phy_ctrl_offset eth0:0x100; eth1:0x200 */
#define ETH_TX_CLK_SEL			BIT(16)
#define ETH_PHY_INTF_SELI		BIT(0)

/* eth_axi_lp_ctrl_offset eth0:0x108; eth1:0x208 */
#define ETH_CSYSREQ_VAL			BIT(0)

/* hsp_aclk_ctrl_offset (0x148) */
#define HSP_ACLK_CLKEN				BIT(31)
#define HSP_ACLK_DIVSOR				(0x2 << 4)

/* hsp_cfg_ctrl_offset (0x14c) */
#define HSP_CFG_CLKEN			BIT(31)
#define SCU_HSP_PCLK_EN			BIT(30)
#define HSP_CFG_CTRL_REGSET		(HSP_CFG_CLKEN | SCU_HSP_PCLK_EN)

/* PHY default addr in mdio*/
#define PHY_ADDR				-1

struct eswin_qos_priv {
	struct device *dev;
	int dev_id;
	struct regmap *crg_regmap;
	struct regmap *hsp_regmap;
	int phyaddr;
	unsigned int dly_hsp_reg[3];
	unsigned int dly_param_1000m[3];
	unsigned int dly_param_100m[3];
	unsigned int dly_param_10m[3];
};

static struct clk *dwc_eth_find_clk(struct plat_stmmacenet_data *plat_dat,
				    const char *name)
{
	for (int i = 0; i < plat_dat->num_clks; i++)
		if (strcmp(plat_dat->clks[i].id, name) == 0)
			return plat_dat->clks[i].clk;

	return NULL;
}

static int dwc_eth_dwmac_config_dt(struct platform_device *pdev,
				   struct plat_stmmacenet_data *plat_dat)
{
	struct device *dev = &pdev->dev;
	u32 burst_map = 0;
	u32 bit_index = 0;
	u32 a_index = 0;

	if (!plat_dat->axi) {
		plat_dat->axi = devm_kzalloc(&pdev->dev, sizeof(struct stmmac_axi), GFP_KERNEL);

		if (!plat_dat->axi)
			return -ENOMEM;
	}

	plat_dat->axi->axi_lpi_en = device_property_read_bool(dev,
							      "snps,en-lpi");
	if (device_property_read_u32(dev, "snps,write-requests",
				     &plat_dat->axi->axi_wr_osr_lmt)) {
		/**
		 * Since the register has a reset value of 1, if property
		 * is missing, default to 1.
		 */
		plat_dat->axi->axi_wr_osr_lmt = 1;
	} else {
		/**
		 * If property exists, to keep the behavior from dwc_eth_qos,
		 * subtract one after parsing.
		 */
		plat_dat->axi->axi_wr_osr_lmt--;
	}

	if (device_property_read_u32(dev, "snps,read-requests",
				     &plat_dat->axi->axi_rd_osr_lmt)) {
		/**
		 * Since the register has a reset value of 1, if property
		 * is missing, default to 1.
		 */
		plat_dat->axi->axi_rd_osr_lmt = 1;
	} else {
		/**
		 * If property exists, to keep the behavior from dwc_eth_qos,
		 * subtract one after parsing.
		 */
		plat_dat->axi->axi_rd_osr_lmt--;
	}
	device_property_read_u32(dev, "snps,burst-map", &burst_map);

	/* converts burst-map bitmask to burst array */
	for (bit_index = 0; bit_index < 7; bit_index++) {
		if (burst_map & (1 << bit_index)) {
			switch (bit_index) {
			case 0:
				plat_dat->axi->axi_blen[a_index] = 4; break;
			case 1:
				plat_dat->axi->axi_blen[a_index] = 8; break;
			case 2:
				plat_dat->axi->axi_blen[a_index] = 16; break;
			case 3:
				plat_dat->axi->axi_blen[a_index] = 32; break;
			case 4:
				plat_dat->axi->axi_blen[a_index] = 64; break;
			case 5:
				plat_dat->axi->axi_blen[a_index] = 128; break;
			case 6:
				plat_dat->axi->axi_blen[a_index] = 256; break;
			default:
				break;
			}
			a_index++;
		}
	}

	/* dwc-qos needs GMAC4, AAL, TSO and PMT */
	plat_dat->has_gmac4 = 1;
	plat_dat->dma_cfg->aal = 1;
	plat_dat->flags |= STMMAC_FLAG_TSO_EN;
	plat_dat->pmt = 1;

	return 0;
}

static int dwc_qos_probe(struct platform_device *pdev,
			 struct plat_stmmacenet_data *plat_dat,
			 struct stmmac_resources *stmmac_res)
{
	plat_dat->pclk = dwc_eth_find_clk(plat_dat, "phy_ref_clk");

	return 0;
}

static void eswin_qos_fix_speed(void *priv, int speed, unsigned int mode)
{
	struct eswin_qos_priv *dwc_priv = priv;
	int i;

	switch (speed) {
	case SPEED_1000:
		for (i = 0; i < 3; i++)
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_1000m[i]);

		break;
	case SPEED_100:
		for (i = 0; i < 3; i++) {
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_100m[i]);
		}

		break;
	case SPEED_10:
		for (i = 0; i < 3; i++) {
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_10m[i]);
		}

		break;
	default:
		dev_err(dwc_priv->dev, "invalid speed %u\n", speed);
		break;
	}
}

static int eswin_qos_probe(struct platform_device *pdev,
			   struct plat_stmmacenet_data *plat_dat,
			   struct stmmac_resources *stmmac_res)
{
	struct eswin_qos_priv *dwc_priv;
	u32 hsp_aclk_ctrl_offset;
	u32 hsp_aclk_ctrl_regset;
	u32 hsp_cfg_ctrl_offset;
	u32 eth_axi_lp_ctrl_offset;
	u32 eth_phy_ctrl_offset;
	u32 eth_phy_ctrl_regset;
	struct clk *clk_app;
	int ret;
	int err;

	dwc_priv = devm_kzalloc(&pdev->dev, sizeof(*dwc_priv), GFP_KERNEL);
	if (!dwc_priv)
		return -ENOMEM;

	if (device_property_read_u32(&pdev->dev, "id", &dwc_priv->dev_id))
		return dev_err_probe(&pdev->dev, -EINVAL,
				"Can not read device id!\n");

	dwc_priv->dev = &pdev->dev;

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,phyaddr", 0,
					 &dwc_priv->phyaddr);
	if (ret)
		dev_warn(&pdev->dev, "can't get phyaddr (%d)\n", ret);

	ret = of_property_read_variable_u32_array(pdev->dev.of_node, "eswin,dly_hsp_reg",
						  &dwc_priv->dly_hsp_reg[0], 3, 0);
	if (ret != 3) {
		dev_err(&pdev->dev, "can't get delay hsp reg.ret(%d)\n", ret);
		return ret;
	}

	ret = of_property_read_variable_u32_array(pdev->dev.of_node, "dly-param-1000m",
						  &dwc_priv->dly_param_1000m[0], 3, 0);
	if (ret != 3) {
		dev_err(&pdev->dev, "can't get delay param for 1Gbps mode (%d)\n", ret);
		return ret;
	}

	ret = of_property_read_variable_u32_array(pdev->dev.of_node, "dly-param-100m",
						  &dwc_priv->dly_param_100m[0], 3, 0);
	if (ret != 3) {
		dev_err(&pdev->dev, "can't get delay param for 100Mbps mode (%d)\n", ret);
		return ret;
	}

	ret = of_property_read_variable_u32_array(pdev->dev.of_node, "dly-param-10m",
						  &dwc_priv->dly_param_10m[0], 3, 0);
	if (ret != 3) {
		dev_err(&pdev->dev, "can't get delay param for 10Mbps mode (%d)\n", ret);
		return ret;
	}

	dwc_priv->crg_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							       "eswin,syscrg_csr");
	if (IS_ERR(dwc_priv->crg_regmap)) {
		dev_dbg(&pdev->dev, "No syscrg_csr phandle specified\n");
		return 0;
	}

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,syscrg_csr", 1,
					 &hsp_aclk_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get syscrg_csr 1\n");

	regmap_read(dwc_priv->crg_regmap, hsp_aclk_ctrl_offset, &hsp_aclk_ctrl_regset);
	hsp_aclk_ctrl_regset |= (HSP_ACLK_CLKEN | HSP_ACLK_DIVSOR);
	regmap_write(dwc_priv->crg_regmap, hsp_aclk_ctrl_offset, hsp_aclk_ctrl_regset);

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,syscrg_csr", 2,
					 &hsp_cfg_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get syscrg_csr 2\n");

	regmap_write(dwc_priv->crg_regmap, hsp_cfg_ctrl_offset, HSP_CFG_CTRL_REGSET);

	dwc_priv->hsp_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							       "eswin,hsp_sp_csr");
	if (IS_ERR(dwc_priv->hsp_regmap)) {
		dev_dbg(&pdev->dev, "No hsp_sp_csr phandle specified\n");
		return 0;
	}

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,hsp_sp_csr", 2,
					 &eth_phy_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get hsp_sp_csr 2\n");

	regmap_read(dwc_priv->hsp_regmap,
		    eth_phy_ctrl_offset,
		    &eth_phy_ctrl_regset);
	eth_phy_ctrl_regset |= (ETH_TX_CLK_SEL | ETH_PHY_INTF_SELI);
	regmap_write(dwc_priv->hsp_regmap,
		     eth_phy_ctrl_offset,
		     eth_phy_ctrl_regset);

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,hsp_sp_csr", 3,
					 &eth_axi_lp_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"can't get hsp_sp_csr 3\n");

	regmap_write(dwc_priv->hsp_regmap,
		     eth_axi_lp_ctrl_offset,
		     ETH_CSYSREQ_VAL);

	clk_app = devm_clk_get_enabled(&pdev->dev, "app");
	if (IS_ERR(clk_app))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk_app),
				"error getting app clock\n");

	plat_dat->clk_tx_i = devm_clk_get_enabled(&pdev->dev, "tx");
	if (IS_ERR(plat_dat->clk_tx_i))
		return dev_err_probe(&pdev->dev, PTR_ERR(plat_dat->clk_tx_i),
				"error getting tx clock\n");

	plat_dat->fix_mac_speed = eswin_qos_fix_speed;
	plat_dat->set_clk_tx_rate = stmmac_set_clk_tx_rate;
	plat_dat->bsp_priv = dwc_priv;
	plat_dat->phy_addr = PHY_ADDR;

	return 0;
}

struct dwc_eth_dwmac_data {
	int (*probe)(struct platform_device *pdev,
		     struct plat_stmmacenet_data *plat_dat,
		     struct stmmac_resources *res);
	const char *stmmac_clk_name;
};

static const struct dwc_eth_dwmac_data eswin_qos_data = {
	.probe = eswin_qos_probe,
	.stmmac_clk_name = "stmmaceth",
};

static int dwc_eth_dwmac_probe(struct platform_device *pdev)
{
	const struct dwc_eth_dwmac_data *data;
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	int ret;

	data = device_get_match_data(&pdev->dev);

	memset(&stmmac_res, 0, sizeof(struct stmmac_resources));

	/**
	 * Since stmmac_platform supports name IRQ only, basic platform
	 * resource initialization is done in the glue logic.
	 */
	stmmac_res.irq = platform_get_irq(pdev, 0);
	if (stmmac_res.irq < 0)
		return stmmac_res.irq;
	stmmac_res.wol_irq = stmmac_res.irq;

	stmmac_res.addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(stmmac_res.addr))
		return PTR_ERR(stmmac_res.addr);

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	plat_dat->stmmac_clk = dwc_eth_find_clk(plat_dat,
						data->stmmac_clk_name);

	if (data->probe)
		ret = data->probe(pdev, plat_dat, &stmmac_res);
	if (ret < 0) {
		return dev_err_probe(&pdev->dev, ret,
				"failed to probe subdriver\n");
	}

	ret = dwc_eth_dwmac_config_dt(pdev, plat_dat);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"Failed to config dt\n");

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"Failed to driver probe\n");

	return ret;
}

static const struct of_device_id dwc_eth_dwmac_match[] = {
	{ .compatible = "eswin,eic7700-qos-eth", .data = &eswin_qos_data },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc_eth_dwmac_match);

static struct platform_driver eic7700_eth_dwmac_driver = {
	.probe  = dwc_eth_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "eic7700-eth-dwmac",
		.pm             = &stmmac_pltfr_pm_ops,
		.of_match_table = dwc_eth_dwmac_match,
	},
};
module_platform_driver(eic7700_eth_dwmac_driver);

MODULE_AUTHOR("Eswin");
MODULE_AUTHOR("Shuang Liang <liangshuang@eswincomputing.com>");
MODULE_AUTHOR("Shangjuan Wei <weishangjuan@eswincomputing.com>");
MODULE_DESCRIPTION("Eswin eic7700 qos ethernet driver");
MODULE_LICENSE("GPL");
