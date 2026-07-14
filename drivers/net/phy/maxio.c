// SPDX-License-Identifier: GPL-2.0-only
/* Driver for Maxio Ethernet PHYs. */

#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/property.h>

#define MAXIO_MAE0621A_PHY_ID		0x7b744412

#define MAXIO_PAGE_SELECT		0x1f
#define MAXIO_MAE0621A_PHYCR2_PAGE	0xa43
#define MAXIO_MAE0621A_PHYCR2		0x19
#define MAXIO_MAE0621A_CLKOUT_125M	BIT(11)
#define MAXIO_MAE0621A_CLKOUT_ENABLE	BIT(0)

struct maxio_priv {
	bool clk_out_125m;
};

static int maxio_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, MAXIO_PAGE_SELECT);
}

static int maxio_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, MAXIO_PAGE_SELECT, page);
}

static int maxio_mae0621a_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct maxio_priv *priv;
	u32 frequency;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	ret = device_property_read_u32(dev, "maxio,clk-out-frequency-hz",
				       &frequency);
	if (ret == -EINVAL)
		return 0;
	if (ret)
		return ret;

	if (frequency != 125000000) {
		phydev_err(phydev, "invalid CLKOUT frequency %u\n", frequency);
		return -EINVAL;
	}

	priv->clk_out_125m = true;

	return 0;
}

static int maxio_mae0621a_config_init(struct phy_device *phydev)
{
	struct maxio_priv *priv = phydev->priv;
	int ret;

	if (!priv->clk_out_125m)
		return 0;

	ret = phy_modify_paged_changed(phydev, MAXIO_MAE0621A_PHYCR2_PAGE,
				       MAXIO_MAE0621A_PHYCR2,
				       MAXIO_MAE0621A_CLKOUT_ENABLE |
				       MAXIO_MAE0621A_CLKOUT_125M,
				       MAXIO_MAE0621A_CLKOUT_ENABLE |
				       MAXIO_MAE0621A_CLKOUT_125M);
	if (ret <= 0)
		return ret;

	return genphy_soft_reset(phydev);
}

static struct phy_driver maxio_drivers[] = {
	{
		PHY_ID_MATCH_EXACT(MAXIO_MAE0621A_PHY_ID),
		.name		= "Maxio MAE0621A",
		.probe		= maxio_mae0621a_probe,
		.config_init	= maxio_mae0621a_config_init,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= maxio_read_page,
		.write_page	= maxio_write_page,
	},
};
module_phy_driver(maxio_drivers);

static const struct mdio_device_id __maybe_unused maxio_tbl[] = {
	{ PHY_ID_MATCH_EXACT(MAXIO_MAE0621A_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, maxio_tbl);

MODULE_AUTHOR("Liu Changjie <liucj1228@outlook.com>");
MODULE_DESCRIPTION("Maxio Ethernet PHY driver");
MODULE_LICENSE("GPL");
