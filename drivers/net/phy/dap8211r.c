// SPDX-License-Identifier: GPL
/*
 * Driver for the DAPU Telecom DAP8211R(I) Gigabit Ethernet PHY.
 *
 * Specifications:
 *   - IEEE 802.3 10BASE-Te, 100BASE-TX, 1000BASE-T
 *   - IEEE 802.3az-2010 Energy Efficient Ethernet
 *   - IEEE 1588 SyncE support
 *   - RGMII
 *
 * Author: Artem Shimko <a.shimko.dev@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/iopoll.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>

#define DAP8211R_PHY_ID			0x0008011B
#define DAP8211R_PHY_ID_MASK		0xFFFFFFFF

#define DAP8211R_EXT_ADD		0x1E
#define DAP8211R_EXT_DATA		0x1F

#define DAP8211R_PHY_CON		0xA001
#define DAP8211R_PHY_SW_RST		BIT(15)

#define DAP8211R_RGMII_CON		0xA003
/* Default initial TX delay value by datasheet. */
#define DAP8211R_INIT_TX_DEL_VAL	1
#define DAP8211R_RGMII_TX_DEL_MASK	GENMASK(3, 0)
#define DAP8211R_RGMII_RX_DEL_MASK	GENMASK(13, 10)
#define DAP8211R_RGMII_CLK_INVERT	BIT(14)

#define DAP8211R_RGMII_CONFIG_MASK	(DAP8211R_RGMII_CLK_INVERT | \
					DAP8211R_RGMII_RX_DEL_MASK | \
					DAP8211R_RGMII_TX_DEL_MASK)

/* Default RGMII delay: 13 * 150 == 1.95ns */
#define DAP8211R_DEFAULT_DEL_SEL	0xD

static const int dap8211r_internal_delay[] = {0, 150, 300, 450, 600, 750, 900,
					      1050, 1200, 1350, 1500, 1650, 1800,
					      1950, 2100, 2250};

#define DAP8211R_DELAY_SIZE	ARRAY_SIZE(dap8211r_internal_delay)

/**
 * dap8211r_read_ext() - Read extended register
 * @phydev: PHY device structure
 * @reg: Extended register address
 *
 * Reads a PHY extended register using the indirect access method.
 * The caller must hold the MDIO bus lock.
 *
 * Return: Register value on success, or negative error code
 */
static int dap8211r_read_ext(struct phy_device *phydev, u16 reg)
{
	int ret;

	phy_lock_mdio_bus(phydev);
	ret = __phy_write(phydev, DAP8211R_EXT_ADD, reg);
	if (ret < 0)
		goto out;

	ret = __phy_read(phydev, DAP8211R_EXT_DATA);
out:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

/**
 * dap8211r_modify_ext() - Modify extended register bits
 * @phydev: PHY device structure
 * @reg: Extended register address
 * @mask: Bit mask of bits to clear
 * @set: Bit mask of bits to set
 *
 * Modifies a PHY extended register using the indirect access method.
 * New value = (old value & ~mask) | set.
 * The caller must hold the MDIO bus lock.
 *
 * Return: 0 on success, or negative error code
 */
static int dap8211r_modify_ext(struct phy_device *phydev, u16 reg, u16 mask, u16 set)
{
	int ret;

	phy_lock_mdio_bus(phydev);
	ret = __phy_write(phydev, DAP8211R_EXT_ADD, reg);
	if (ret < 0)
		goto out;

	ret = __phy_modify(phydev, DAP8211R_EXT_DATA, mask, set);
out:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

/**
 * dap8211r_config_init() - Initialize PHY
 * @phydev: PHY device structure
 *
 * Configures the PHY during initialization:
 * - TX clock invertion
 * - RGMII delays based on interface mode
 * - Software reset to apply settings (low active, self clear)
 *
 * Return: 0 on success, or negative error code
 */
static int dap8211r_config_init(struct phy_device *phydev)
{
	struct device_node *phydev_node = phydev->mdio.dev.of_node;
	u16 set = 0, val = 0;
	int ret;
	s32 internal_delay;

	if (of_property_read_bool(phydev_node, "dapu,tx-inverted-clk"))
		set |= DAP8211R_RGMII_CLK_INVERT;

	internal_delay = phy_get_internal_delay(phydev, dap8211r_internal_delay,
						DAP8211R_DELAY_SIZE, true);
	if (internal_delay < 0) {
		if (phydev->interface == PHY_INTERFACE_MODE_RGMII)
			internal_delay = 0;
		else
			internal_delay = DAP8211R_DEFAULT_DEL_SEL;
	}

	set |= FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK, internal_delay);

	internal_delay = phy_get_internal_delay(phydev, dap8211r_internal_delay,
						DAP8211R_DELAY_SIZE, false);
	if (internal_delay < 0) {
		if (phydev->interface == PHY_INTERFACE_MODE_RGMII)
			internal_delay = DAP8211R_INIT_TX_DEL_VAL;
		else
			internal_delay = DAP8211R_DEFAULT_DEL_SEL;
	}

	set |= FIELD_PREP(DAP8211R_RGMII_TX_DEL_MASK, internal_delay);

	ret = dap8211r_modify_ext(phydev, DAP8211R_PHY_CON, DAP8211R_PHY_SW_RST, 0);
	if (ret)
		return ret;

	/* Wait for reset self-clear (max 200 us) */
	ret = read_poll_timeout(dap8211r_read_ext, val,
				(val & DAP8211R_PHY_SW_RST),
				20, 200, false, phydev, DAP8211R_PHY_CON);
	if (ret)
		return ret;

	ret = dap8211r_modify_ext(phydev, DAP8211R_RGMII_CON, DAP8211R_RGMII_CONFIG_MASK, set);
	if (ret)
		return ret;

	return 0;
}

static struct phy_driver dap8211r_driver[] = {
	{
		PHY_ID_MATCH_EXACT(DAP8211R_PHY_ID),
		.name		= "DAP8211R Gigabit Ethernet",
		.config_init	= dap8211r_config_init,
		.read_status	= genphy_read_status,
		.set_loopback	= genphy_loopback,
		.config_aneg	= genphy_config_aneg,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	},
};
module_phy_driver(dap8211r_driver);

MODULE_DESCRIPTION("DAP8211R Gigabit Ethernet PHY driver");
MODULE_AUTHOR("Artem Shimko <a.shimko.dev@gmail.com>");
MODULE_LICENSE("GPL");

static const struct mdio_device_id __maybe_unused dap8211r_tb[] = {
	{ DAP8211R_PHY_ID, DAP8211R_PHY_ID_MASK },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(mdio, dap8211r_tb);

