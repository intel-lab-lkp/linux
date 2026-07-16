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
#define DAP8211R_RGMII_TX_DEL_MASK	GENMASK(3, 0)
#define DAP8211R_RGMII_RX_DEL_MASK	GENMASK(13, 10)
#define DAP8211R_RGMII_CLK_INVERT	BIT(14)

/* Default RGMII delay: 13 * 150 == 1.95ns */
#define DAP8211R_DEFAULT_DELAY_SEL	0xD

struct dap8211r_delay_config {
	u32 ps;
	u8 sel;
};

static const struct dap8211r_delay_config delay_config[] = {
	{   0, 0},
	{ 150, 1},
	{ 300, 2},
	{ 450, 3},
	{ 600, 4},
	{ 750, 5},
	{ 900, 6},
	{1050, 7},
	{1200, 8},
	{1350, 9},
	{1500, 10},
	{1650, 11},
	{1800, 12},
	{1950, 13},
	{2100, 14},
	{2250, 15},
};

#define DAP8211R_DELAY_COUNT	ARRAY_SIZE(delay_config)

/**
 * dap8211r_delay_ps_to_sel() - Convert ps to register value (exact match only)
 * @ps: Delay in picoseconds
 *
 * Converts a delay value in picoseconds to the corresponding register value
 * for RGMII delay configuration. The PHY supports specific values from
 * 0 to 2250 ps in 150 ps steps.
 *
 * Return: Register value (0-15) on success, -EINVAL if @ps is not supported.
 */

static int dap8211r_delay_ps_to_sel(u32 ps)
{
	for (int i = 0; i < DAP8211R_DELAY_COUNT; i++)
		if (ps == delay_config[i].ps)
			return delay_config[i].sel;

	return -EINVAL;
}

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
 * dap8211r_get_rgmii_delay() - Get RGMII delay from DT
 * @phydev: PHY device
 * @prop_name: DT property name
 * @is_id: If phy mode is PHY_INTERFACE_MODE_RGMII_[TXID,RXID,ID]
 *
 * Reads the RGMII delay from the device tree. If the property is not
 * specified, the default delay (1950ps) is used.
 *
 * Return: Register value (0-15) on success, negative error code on failure.
 *	   -EINVAL: Property not specified and is_id is false.
 */
static int dap8211r_get_rgmii_delay(struct phy_device *phydev, const char *prop_name, bool is_id)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	u32 ps = 0;
	int ret;

	ret = of_property_read_u32(np, prop_name, &ps);
	if (ret == -EINVAL)
		return (is_id) ? DAP8211R_DEFAULT_DELAY_SEL : ret;
	if (ret < 0)
		return ret;

	return dap8211r_delay_ps_to_sel(ps);
}

/**
 * dap8211r_config_init() - Initialize PHY
 * @phydev: PHY device structure
 *
 * Configures the PHY during initialization:
 * - RGMII delays based on interface mode
 * - TX clock invertion
 * - Software reset to apply settings (low active, self clear)
 *
 * Return: 0 on success, or negative error code
 */
static int dap8211r_config_init(struct phy_device *phydev)
{
	struct device_node *phydev_node = phydev->mdio.dev.of_node;
	u16 mask = 0, set = 0;
	int ret, retries = 10;

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		ret = dap8211r_get_rgmii_delay(phydev, "rx-internal-delay-ps", false);
		if (ret >= 0) {
			set = FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK, ret);
			mask = DAP8211R_RGMII_RX_DEL_MASK;
		} else if ((ret < 0) && (ret != -EINVAL)) {
			return ret;
		}

		ret = dap8211r_get_rgmii_delay(phydev, "tx-internal-delay-ps", false);
		if (ret >= 0) {
			set |= FIELD_PREP(DAP8211R_RGMII_TX_DEL_MASK, ret);
			mask |= DAP8211R_RGMII_TX_DEL_MASK;
		} else if ((ret < 0) && (ret != -EINVAL)) {
			return ret;
		}
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		ret = dap8211r_get_rgmii_delay(phydev, "rx-internal-delay-ps", true);
		if (ret < 0)
			return ret;

		set = FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK, ret);
		mask = DAP8211R_RGMII_RX_DEL_MASK;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		ret = dap8211r_get_rgmii_delay(phydev, "rx-internal-delay-ps", true);
		if (ret < 0)
			return ret;

		set = FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK, ret);
		mask = DAP8211R_RGMII_RX_DEL_MASK;
		fallthrough;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		ret = dap8211r_get_rgmii_delay(phydev, "tx-internal-delay-ps", true);
		if (ret < 0)
			return ret;

		set |= FIELD_PREP(DAP8211R_RGMII_TX_DEL_MASK, ret);
		mask |= DAP8211R_RGMII_TX_DEL_MASK;
		break;
	default:
		phydev_err(phydev, "Unsupported interface: %d\n",
			   phydev->interface);
		return -EINVAL;
	}

	if (of_property_read_bool(phydev_node, "dapu,tx-inverted-clk"))
		set |= DAP8211R_RGMII_CLK_INVERT;

	mask |= DAP8211R_RGMII_CLK_INVERT;

	ret = dap8211r_modify_ext(phydev, DAP8211R_PHY_CON, DAP8211R_PHY_SW_RST, 0);
	if (ret)
		return ret;

	/* Wait for reset self-clear */
	do {
		fsleep(20);
		ret = dap8211r_read_ext(phydev, DAP8211R_PHY_CON);
		if (ret < 0)
			return ret;
	} while (!(ret & DAP8211R_PHY_SW_RST) && --retries);

	if (!retries)
		return -ETIMEDOUT;

	ret = dap8211r_modify_ext(phydev, DAP8211R_RGMII_CON, mask, set);
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
