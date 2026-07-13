// SPDX-License-Identifier: GPL
/*
 * Driver for the DAPU Telecom DAP8211R(I) Gigabit Ethernet PHY.
 *
 * Specifications:
 *   - IEEE 802.3 10BASE-Te, 100BASE-TX, 1000BASE-T
 *   - IEEE 802.3az-2010 Energy Efficient Ethernet
 *   - IEEE 1588 SyncE support
 *   - RGMII
 *   - Package Generator for diagnostics
 *
 * Author: Artem Shimko <a.shimko.dev@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
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

#define DAP8211R_PKGC5			0xA0
#define DAP8211R_PKG_PL_MASK		GENMASK(1, 0)
#define DAP8211R_PKG_PL_5AA5		BIT(1)
#define DAP8211R_PKG_COR_CRC		BIT(2)
#define DAP8211R_PKG_GEN_EN		BIT(12)
#define DAP8211R_PKG_GEN_MODE		BIT(13)
#define DAP8211R_PKG_GATE_EN		BIT(14)
#define DAP8211R_PKG_CHK_EN		BIT(15)
#define DAP8211R_PKG_GEN_MASK		GENMASK(15, 12)

#define DAP8211R_PHY_CON		0xA001
#define DAP8211R_PHY_LDO_EN		BIT(6)
#define DAP8211R_PHY_RX_DLY		BIT(8)
#define DAP8211R_PHY_GATE_RX_CLK	BIT(9)
#define DAP8211R_PHY_SW_RST		BIT(15)

#define DAP8211R_RGMII_CON		0xA003
#define DAP8211R_RGMII_TX_DEL_MASK	GENMASK(3, 0)
#define DAP8211R_RGMII_RX_DEL_MASK	GENMASK(13, 10)
#define DAP8211R_RGMII_CLK_INVERT	BIT(14)

/* Default RGMII delay (1.95ns == 13 * 150)ps) */
#define DAP8211R_DEFAULT_DELAY_PS	1950

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

struct dap8211r_priv {
	struct device *dev;
#if IS_ENABLED(CONFIG_DEBUG_FS)
	struct dentry *root_dir;
	bool crc_cor;
	bool loopback;
#endif
};

/**
 * dap8211r_delay_ps_to_sel() - Convert picoseconds to register value
 * @ps: Delay in picoseconds (0-2250)
 *
 * Converts a delay value in picoseconds to the corresponding register value
 * for RGMII delay configuration. The PHY supports 150ps steps from 0 to 2250ps.
 *
 * Return: Register value (0-15)
 */
static u16 dap8211r_delay_ps_to_sel(struct phy_device *phydev, u32 ps)
{
	int i, best_idx = 0;
	u32 best_diff = UINT_MAX, diff = 0;

	if (!ps)
		return 0;

	for (i = 0; i < DAP8211R_DELAY_COUNT; i++) {
		diff = abs(ps - delay_config[i].ps);

		if (diff < best_diff) {
			best_diff = diff;
			best_idx = i;
		}

		if (!diff)
			break;
	}

	if (best_diff)
		phydev_warn(phydev, "Delay %u ps not found, using closest %u ps\n", ps,
			    delay_config[best_idx].ps);

	phydev_dbg(phydev, "Delay ps idx: %u\n", delay_config[best_idx].sel);

	return delay_config[best_idx].sel;
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
 * dap8211r_get_rgmii_delay() - Get RGMII delay from device tree
 * @phydev: PHY device structure
 * @prop_name: Device tree property name
 *
 * Reads the RGMII delay from the device tree. If the property is not
 * specified, the default delay (1950ps) is used.
 *
 * Return: Register value (0-15) or default if property not found
 */
static u32 dap8211r_get_rgmii_delay(struct phy_device *phydev, const char *prop_name)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	int ret;
	u32 ps = 0;

	ret = of_property_read_u32(np, prop_name, &ps);
	if (ret) {
		phydev_dbg(phydev, "Using default delay (%ups)\n", DAP8211R_DEFAULT_DELAY_PS);
		ps = DAP8211R_DEFAULT_DELAY_PS;
	}

	return dap8211r_delay_ps_to_sel(phydev, ps);
}

/**
 * dap8211r_config_init() - Initialize PHY
 * @phydev: PHY device structure
 *
 * Configures the PHY during initialization:
 * - RGMII delays based on interface mode
 * - TX clock invertion
 * - Software reset to apply settings
 *
 * Return: 0 on success, or negative error code
 */
static int dap8211r_config_init(struct phy_device *phydev)
{
	struct device_node *phydev_node = phydev->mdio.dev.of_node;
	u16 mask = 0, set = 0;
	int ret;

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		set = FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK,
				 dap8211r_get_rgmii_delay(phydev, "rx-internal-delay-ps"));
		mask = DAP8211R_RGMII_RX_DEL_MASK;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		set = FIELD_PREP(DAP8211R_RGMII_RX_DEL_MASK,
				 dap8211r_get_rgmii_delay(phydev, "rx-internal-delay-ps"));
		mask = DAP8211R_RGMII_RX_DEL_MASK;
		fallthrough;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		set |= FIELD_PREP(DAP8211R_RGMII_TX_DEL_MASK,
				  dap8211r_get_rgmii_delay(phydev, "tx-internal-delay-ps"));
		mask |= DAP8211R_RGMII_TX_DEL_MASK;
		break;
	default:
		phydev_err(phydev, "Unsupported interface: %d\n",
			   phydev->interface);
		return -EINVAL;
	}

	if (of_property_read_bool(phydev_node, "tx-use-inverted-clk"))
		set |= DAP8211R_RGMII_CLK_INVERT;

	mask |= DAP8211R_RGMII_CLK_INVERT;

	ret = dap8211r_modify_ext(phydev, DAP8211R_PHY_CON, DAP8211R_PHY_SW_RST, 0);
	if (ret)
		return ret;

	/* Wait for reset self-clear */
	fsleep(200);

	ret = dap8211r_modify_ext(phydev, DAP8211R_RGMII_CON, mask, set);
	if (ret)
		return ret;

	phydev_dbg(phydev, "RGMII configured: interface=%d, mask=0x%04x, set=0x%04x\n",
		   phydev->interface, mask, set);

	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)

/**
 * info_show() - Show PHY status information
 * @sf: Sequence file structure
 * @data: Private data (struct dap8211r_priv *)
 *
 * Displays PHY configuration and status registers for debugging.
 *
 * Return: 0 on success, or negative error code
 */
static int info_show(struct seq_file *sf, void *data)
{
	struct dap8211r_priv *priv = sf->private;
	struct phy_device *phydev = to_phy_device(priv->dev);
	int val;

	val = dap8211r_read_ext(phydev, DAP8211R_PHY_CON);
	if (val < 0)
		return val;

	seq_printf(sf, "PHY_CON: 0x%04x\n", val);
	seq_printf(sf, "  LDO enabled: %s\n", FIELD_GET(DAP8211R_PHY_LDO_EN, val)  ?
		   "yes" : "no");
	seq_printf(sf, "  RX dly en: %s\n", FIELD_GET(DAP8211R_PHY_RX_DLY, val) ?
		   "yes" : "no");
	seq_printf(sf, "  RX ckl gating: %s\n", FIELD_GET(DAP8211R_PHY_GATE_RX_CLK, val) ?
		   "yes" : "no");

	val = dap8211r_read_ext(phydev, DAP8211R_RGMII_CON);
	if (val < 0)
		return val;

	seq_printf(sf, "RGMII_CON: 0x%04x\n", val);
	seq_printf(sf, "  TX delay idx: %lx\n", FIELD_GET(DAP8211R_RGMII_TX_DEL_MASK, val));
	seq_printf(sf, "  RX delay idx: %lx\n", FIELD_GET(DAP8211R_RGMII_RX_DEL_MASK, val));
	seq_printf(sf, "  CLK invert: %s\n", FIELD_GET(DAP8211R_RGMII_CLK_INVERT, val) ?
		   "yes" : "no");

	val = phy_read(phydev, MII_BMSR);
	if (val < 0)
		return val;

	seq_printf(sf, "BMSR: 0x%04x\n", val);
	seq_printf(sf, "  Link: %s\n", FIELD_GET(BMSR_LSTATUS, val) ?
		   "up" : "down");
	seq_printf(sf, "  AN complete: %s\n", FIELD_GET(BMSR_ANEGCOMPLETE, val) ?
		   "yes" : "no");

	val = dap8211r_read_ext(phydev, DAP8211R_PKGC5);
	if (val < 0)
		return val;

	if (FIELD_GET(DAP8211R_PKG_GEN_EN, val) &&
	    FIELD_GET(DAP8211R_PKG_CHK_EN, val))
		seq_puts(sf, "WARNING: Package Generating enabled\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

/**
 * pkg_gen_show() - Show packet generator status
 * @sf: Sequence file structure
 * @data: Private data (struct dap8211r_priv *)
 *
 * Displays the current status of the internal packet generator,
 * including CRC corruption and loopback settings.
 *
 * Return: 0 on success, or negative error code
 */
static int pkg_gen_show(struct seq_file *sf, void *data)
{
	struct dap8211r_priv *priv = sf->private;
	struct phy_device *phydev = to_phy_device(priv->dev);
	int val;

	val = dap8211r_read_ext(phydev, DAP8211R_PKGC5);
	if (val < 0)
		return val;

	seq_puts(sf, "Package Generating: ");
	if (FIELD_GET(DAP8211R_PKG_GEN_EN, val) &&
	    FIELD_GET(DAP8211R_PKG_CHK_EN, val))
		seq_puts(sf, "enabled\n");
	else
		seq_puts(sf, "disabled\n");

	seq_printf(sf, "CRC corruption en: %s\n", FIELD_GET(DAP8211R_PKG_COR_CRC, val) ?
		   "enabled" : "disabled");

	val = phy_read(phydev, MII_BMCR);
	if (val < 0)
		return val;

	seq_printf(sf, "Loopback en: %s\n", FIELD_GET(BMCR_LOOPBACK, val) ?
		   "enabled" : "disabled");
	return 0;
}

/**
 * pkg_gen_write() - Enable/disable packet generator
 * @file: File structure
 * @user_buf: User space buffer
 * @count: Buffer size
 * @ppos: File position
 *
 * Enables or disables the internal packet generator.
 * Also controls loopback and CRC corruption via debugfs flags.
 *
 * NOTE: We intentionally do not use genphy_loopback() here.
 * genphy_loopback() does a full BMCR overwrite (mask = ~0) and
 * waits up to 500 ms for link in loopback mode. This is too
 * heavy for a diagnostic packet generator:
 *
 * - It destroys all other BMCR state (speed, duplex, AN config).
 * - The 500 ms link poll is unnecessary: we already have a link
 *   or are deliberately setting up a test condition.
 * - We want to minimize changes to BMCR during debugging.
 *
 * Return: Number of bytes written on success, or negative error code
 */
static ssize_t pkg_gen_write(struct file *file,
			     const char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	struct seq_file *sf = file->private_data;
	struct dap8211r_priv *priv = sf->private;
	struct phy_device *phydev = to_phy_device(priv->dev);
	bool en;
	u16 mask, set, set_bmcr;
	int ret;

	ret = kstrtobool_from_user(user_buf, count, &en);
	if (ret)
		return ret;

	if (en) {
		set = DAP8211R_PKG_GEN_EN | DAP8211R_PKG_CHK_EN | DAP8211R_PKG_PL_5AA5;

		if (priv->crc_cor)
			set |= DAP8211R_PKG_COR_CRC;

		if (priv->loopback)
			set_bmcr = BMCR_LOOPBACK;
		else
			set_bmcr = BMCR_ANENABLE;
	} else {
		set = DAP8211R_PKG_GEN_MODE | DAP8211R_PKG_GATE_EN;
		set_bmcr = BMCR_ANENABLE;
	}

	ret = phy_modify(phydev, MII_BMCR, BMCR_LOOPBACK | BMCR_ANENABLE, set_bmcr);
	if (ret < 0)
		return ret;

	mask = DAP8211R_PKG_GEN_MASK | DAP8211R_PKG_PL_MASK | DAP8211R_PKG_COR_CRC;
	ret = dap8211r_modify_ext(phydev, DAP8211R_PKGC5, mask, set);
	if (ret < 0) {
		phy_modify(phydev, MII_BMCR, BMCR_LOOPBACK, 0);
		return ret;
	}

	return count;
}
DEFINE_SHOW_STORE_ATTRIBUTE(pkg_gen);

/**
 * dap8211r_debug_init() - Initialize debugfs entries
 * @priv: Private driver data
 *
 * Creates debugfs directory and files for PHY debugging.
 * - info: PHY status information
 * - pkg_gen: Enable/disable generator (1/0)
 * - pkg_gen_crc_cor: Enable CRC corruption (Y/N)
 * - pkg_gen_loopback: Enable loopback (Y/N)
 *
 * Debugfs is only available when CONFIG_DEBUG_FS is enabled.
 */
static void dap8211r_debug_init(struct dap8211r_priv *priv)
{
	priv->root_dir = debugfs_create_dir(dev_name(priv->dev), NULL);
	if (IS_ERR(priv->root_dir))
		return;

	debugfs_create_file("info", 0444, priv->root_dir, priv,
			    &info_fops);
	debugfs_create_file("pkg_gen", 0644, priv->root_dir, priv,
			    &pkg_gen_fops);
	debugfs_create_bool("pkg_gen_crc_cor", 0644, priv->root_dir,
			    &priv->crc_cor);
	debugfs_create_bool("pkg_gen_loopback", 0644, priv->root_dir,
			    &priv->loopback);
}

/**
 * dap8211r_debug_remove() - Remove debugfs entries
 * @priv: Private driver data
 *
 * Recursively removes all debugfs files and directories created
 * by dap8211r_debug_init().
 */
static void dap8211r_debug_remove(struct dap8211r_priv *priv)
{
	debugfs_remove_recursive(priv->root_dir);
}

#else /* !CONFIG_DEBUG_FS */
static void dap8211r_debug_init(struct dap8211r_priv *priv) {}
static void dap8211r_debug_remove(struct dap8211r_priv *priv) {}
#endif /* CONFIG_DEBUG_FS */

/**
 * dap8211r_probe() - PHY probe callback
 * @phydev: PHY device structure
 *
 * Called when the PHY is discovered. Allocates and initializes
 * private driver data and debugfs entries.
 *
 * Return: 0 on success, or negative error code
 */
static int dap8211r_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct dap8211r_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	phydev->priv = priv;

	dap8211r_debug_init(priv);
	phydev_dbg(phydev, "DAP8211R PHY probed\n");

	return 0;
}

/**
 * dap8211r_remove() - PHY remove callback
 * @phydev: PHY device structure
 *
 * Called when the PHY is removed. Cleans up private driver data
 * and debugfs entries.
 */
static void dap8211r_remove(struct phy_device *phydev)
{
	dap8211r_debug_remove(phydev->priv);
}

static struct phy_driver dap8211r_driver[] = {
	{
		PHY_ID_MATCH_EXACT(DAP8211R_PHY_ID),
		.name		= "DAP8211R Gigabit Ethernet",
		.probe          = dap8211r_probe,
		.remove		= dap8211r_remove,
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
