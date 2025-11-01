// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/lockdep.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/slab.h>
#include <linux/types.h>

#define EQ5_PHY_COUNT	2

#define EQ5_PHY0_GP	0x128
#define EQ5_PHY1_GP	0x12c
#define EQ5_PHY0_SGMII	0x134
#define EQ5_PHY1_SGMII	0x138

#define EQ5_GP_TX_SWRST_DIS	BIT(0)		// Tx SW reset
#define EQ5_GP_TX_M_CLKE	BIT(1)		// Tx M clock enable
#define EQ5_GP_SYS_SWRST_DIS	BIT(2)		// Sys SW reset
#define EQ5_GP_SYS_M_CLKE	BIT(3)		// Sys clock enable
#define EQ5_GP_SGMII_MODE	BIT(4)		// SGMII mode
#define EQ5_GP_RGMII_DRV	GENMASK(8, 5)	// RGMII drive strength

#define EQ5_SGMII_PWR_EN	BIT(0)
#define EQ5_SGMII_RST_DIS	BIT(1)
#define EQ5_SGMII_PLL_EN	BIT(2)
#define EQ5_SGMII_SIG_DET_SW	BIT(3)
#define EQ5_SGMII_PWR_STATE	BIT(4)
#define EQ5_SGMII_PLL_ACK	BIT(18)
#define EQ5_SGMII_PWR_STATE_ACK	GENMASK(24, 20)

struct eq5_phy_inst {
	struct eq5_phy_private	*priv;
	struct phy		*phy;
	void __iomem		*gp, *sgmii;
	phy_interface_t		phy_interface;
};

struct eq5_phy_private {
	struct device		*dev;
	struct eq5_phy_inst	phys[EQ5_PHY_COUNT];
};

static int eq5_phy_init(struct phy *phy)
{
	struct eq5_phy_inst *inst = phy_get_drvdata(phy);
	struct eq5_phy_private *priv = inst->priv;
	struct device *dev = priv->dev;
	u32 reg;

	dev_dbg(dev, "phy_init(inst=%td)\n", inst - priv->phys);

	writel(0, inst->gp);
	writel(0, inst->sgmii);

	udelay(5);

	reg = readl(inst->gp) | EQ5_GP_TX_SWRST_DIS | EQ5_GP_TX_M_CLKE |
	      EQ5_GP_SYS_SWRST_DIS | EQ5_GP_SYS_M_CLKE |
	      FIELD_PREP(EQ5_GP_RGMII_DRV, 0x9);
	writel(reg, inst->gp);

	return 0;
}

static int eq5_phy_exit(struct phy *phy)
{
	struct eq5_phy_inst *inst = phy_get_drvdata(phy);
	struct eq5_phy_private *priv = inst->priv;
	struct device *dev = priv->dev;

	dev_dbg(dev, "phy_exit(inst=%td)\n", inst - priv->phys);

	writel(0, inst->gp);
	writel(0, inst->sgmii);
	udelay(5);

	return 0;
}

static int eq5_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct eq5_phy_inst *inst = phy_get_drvdata(phy);
	struct eq5_phy_private *priv = inst->priv;
	struct device *dev = priv->dev;

	dev_dbg(dev, "phy_set_mode(inst=%td, mode=%d, submode=%d)\n",
		inst - priv->phys, mode, submode);

	if (mode != PHY_MODE_ETHERNET)
		return -EOPNOTSUPP;

	if (!phy_interface_mode_is_rgmii(submode) &&
	    submode != PHY_INTERFACE_MODE_SGMII)
		return -EOPNOTSUPP;

	inst->phy_interface = submode;
	return 0;
}

static int eq5_phy_power_on(struct phy *phy)
{
	struct eq5_phy_inst *inst = phy_get_drvdata(phy);
	struct eq5_phy_private *priv = inst->priv;
	struct device *dev = priv->dev;
	u32 reg;

	dev_dbg(dev, "phy_power_on(inst=%td)\n", inst - priv->phys);

	if (inst->phy_interface == PHY_INTERFACE_MODE_SGMII) {
		writel(readl(inst->gp) | EQ5_GP_SGMII_MODE, inst->gp);

		reg = EQ5_SGMII_PWR_EN | EQ5_SGMII_RST_DIS | EQ5_SGMII_PLL_EN;
		writel(reg, inst->sgmii);

		if (readl_poll_timeout(inst->sgmii, reg,
				       reg & EQ5_SGMII_PLL_ACK, 1, 100)) {
			dev_err(dev, "PLL timeout\n");
			return -ETIMEDOUT;
		}

		reg = readl(inst->sgmii);
		reg |= EQ5_SGMII_PWR_STATE | EQ5_SGMII_SIG_DET_SW;
		writel(reg, inst->sgmii);
	} else {
		writel(readl(inst->gp) & ~EQ5_GP_SGMII_MODE, inst->gp);
		writel(0, inst->sgmii);
	}

	return 0;
}

static int eq5_phy_power_off(struct phy *phy)
{
	struct eq5_phy_inst *inst = phy_get_drvdata(phy);
	struct eq5_phy_private *priv = inst->priv;
	struct device *dev = priv->dev;

	dev_dbg(dev, "phy_power_off(inst=%td)\n", inst - priv->phys);

	writel(readl(inst->gp) & ~EQ5_GP_SGMII_MODE, inst->gp);
	writel(0, inst->sgmii);

	return 0;
}

static const struct phy_ops eq5_phy_ops = {
	.init		= eq5_phy_init,
	.exit		= eq5_phy_exit,
	.set_mode	= eq5_phy_set_mode,
	.power_on	= eq5_phy_power_on,
	.power_off	= eq5_phy_power_off,
};

static struct phy *eq5_phy_xlate(struct device *dev,
				 const struct of_phandle_args *args)
{
	struct eq5_phy_private *priv = dev_get_drvdata(dev);

	if (args->args_count != 1 || args->args[0] > 1)
		return ERR_PTR(-EINVAL);

	return priv->phys[args->args[0]].phy;
}

static int eq5_phy_probe_phy(struct eq5_phy_private *priv, unsigned int index,
			     void __iomem *base, unsigned int gp,
			     unsigned int sgmii)
{
	struct eq5_phy_inst *inst = &priv->phys[index];
	struct device *dev = priv->dev;
	struct phy *phy;

	phy = devm_phy_create(dev, dev->of_node, &eq5_phy_ops);
	if (IS_ERR(phy)) {
		dev_err(dev, "failed to create PHY %u\n", index);
		return PTR_ERR(phy);
	}

	inst->priv = priv;
	inst->phy = phy;
	inst->gp = base + gp;
	inst->sgmii = base + sgmii;
	inst->phy_interface = PHY_INTERFACE_MODE_NA;
	phy_set_drvdata(phy, inst);

	return 0;
}

static int eq5_phy_probe(struct auxiliary_device *adev,
			 const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct phy_provider *provider;
	struct eq5_phy_private *priv;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	dev_set_drvdata(dev, priv);

	base = (void __iomem *)dev_get_platdata(dev);

	ret = eq5_phy_probe_phy(priv, 0, base, EQ5_PHY0_GP, EQ5_PHY0_SGMII);
	if (ret)
		return ret;

	ret = eq5_phy_probe_phy(priv, 1, base, EQ5_PHY1_GP, EQ5_PHY1_SGMII);
	if (ret)
		return ret;

	provider = devm_of_phy_provider_register(dev, eq5_phy_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "registering provider failed\n");
		return PTR_ERR(provider);
	}

	return 0;
}

static const struct auxiliary_device_id eq5_phy_id_table[] = {
	{ .name = "clk_eyeq.phy" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, eq5_phy_id_table);

static struct auxiliary_driver eq5_phy_driver = {
	.probe = eq5_phy_probe,
	.id_table = eq5_phy_id_table,
};
module_auxiliary_driver(eq5_phy_driver);

MODULE_DESCRIPTION("EyeQ5 Ethernet PHY driver");
MODULE_AUTHOR("Théo Lebrun <theo.lebrun@bootlin.com>");
MODULE_LICENSE("GPL");
