// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2023 Nuvoton Technology corporation.

#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"

#define IND_AC_BA_REG		0x1FE
#define SR_MII_CTRL		0x3E0000

#define PCS_SR_MII_CTRL_REG	0x0
#define PCS_SPEED_SELECT6	BIT(6)
#define PCS_AN_ENABLE		BIT(12)
#define PCS_SPEED_SELECT13	BIT(13)
#define PCS_RST			BIT(15)

#define PCS_MASK_SPEED		0xDFBF

struct npcm_dwmac {
	void __iomem	*reg;
};

static void npcm_dwmac_fix_mac_speed(void *priv, unsigned int speed,
				     unsigned int mode)
{
	struct npcm_dwmac *dwmac = priv;
	u16 val;

	iowrite16((u16)(SR_MII_CTRL >> 9), dwmac->reg + IND_AC_BA_REG);
	val = ioread16(dwmac->reg + PCS_SR_MII_CTRL_REG);
	val &= PCS_MASK_SPEED;

	switch (speed) {
	case SPEED_1000:
		val |= PCS_SPEED_SELECT6;
		break;
	case SPEED_100:
		val |= PCS_SPEED_SELECT13;
		break;
	case SPEED_10:
		break;
	}

	iowrite16(val, dwmac->reg + PCS_SR_MII_CTRL_REG);
}

void npcm_dwmac_pcs_init(struct npcm_dwmac *dwmac, struct device *dev,
			 struct plat_stmmacenet_data *plat_dat)
{
	u16 val;

	iowrite16((u16)(SR_MII_CTRL >> 9), dwmac->reg + IND_AC_BA_REG);
	val = ioread16(dwmac->reg + PCS_SR_MII_CTRL_REG);
	val |= PCS_RST;
	iowrite16(val, dwmac->reg + PCS_SR_MII_CTRL_REG);

	while (val & PCS_RST)
		val = ioread16(dwmac->reg + PCS_SR_MII_CTRL_REG);

	val &= ~(PCS_AN_ENABLE);
	iowrite16(val, dwmac->reg + PCS_SR_MII_CTRL_REG);
}

static int npcm_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct npcm_dwmac *dwmac;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		ret = -ENOMEM;

	dwmac->reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(dwmac->reg))
		ret = PTR_ERR(dwmac->reg);

	npcm_dwmac_pcs_init(dwmac, &pdev->dev, plat_dat);

	plat_dat->has_gmac = true;
	plat_dat->bsp_priv = dwmac;
	plat_dat->fix_mac_speed = npcm_dwmac_fix_mac_speed;

	return stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
}

static const struct of_device_id npcm_dwmac_match[] = {
	{ .compatible = "nuvoton,npcm8xx-sgmii" },
	{ }
};
MODULE_DEVICE_TABLE(of, npcm_dwmac_match);

static struct platform_driver npcm_dwmac_driver = {
	.probe  = npcm_dwmac_probe,
	.remove_new = stmmac_pltfr_remove,
	.driver = {
		.name           = "npcm-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = npcm_dwmac_match,
	},
};
module_platform_driver(npcm_dwmac_driver);

MODULE_AUTHOR("Tomer Maimon <tomer.maimon@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton NPCM DWMAC glue layer");
MODULE_LICENSE("GPL v2");
