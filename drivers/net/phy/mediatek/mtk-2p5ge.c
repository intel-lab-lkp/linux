// SPDX-License-Identifier: GPL-2.0+
#include <linux/bitfield.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/phy.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "mtk.h"

#define MT7988_2P5GE_PMB "mediatek/mt7988/i2p5ge-phy-pmb.bin"

#define MD32_EN			BIT(0)
#define PMEM_PRIORITY		BIT(8)
#define DMEM_PRIORITY		BIT(16)

#define BASE100T_STATUS_EXTEND		(0x10)
#define BASE1000T_STATUS_EXTEND		(0x11)
#define EXTEND_CTRL_AND_STATUS		(0x16)

#define PHY_AUX_CTRL_STATUS		(0x1d)
#define   PHY_AUX_DPX_MASK		GENMASK(5, 5)
#define   PHY_AUX_SPEED_MASK		GENMASK(4, 2)

/* MTK_PHY_LINK_STATUS_MISC's fields */
#define   MTK_PHY_FDX_ENABLE		BIT(5)

#define MTK_PHY_LPI_PCS_DSP_CTRL		(0x121)
#define   MTK_PHY_LPI_SIG_EN_LO_THRESH100_MASK	GENMASK(12, 8)

/* Registers on MTK phy page 1*/
#define MTK_PHY_PAGE_EXTENDED_1			0x0001
#define MTK_PHY_AUX_CTRL_AND_STATUS		(0x14)
#define   MTK_PHY_ENABLE_DOWNSHIFT		BIT(4)

/* Registers on Token Ring debug nodes */
/* ch_addr = 0x0, node_addr = 0xf, data_addr = 0x3c */
#define AUTO_NP_10XEN				BIT(6)

struct mtk_i2p5ge_phy_priv {
	bool fw_loaded;
	unsigned long led_state;
};

enum {
	PHY_AUX_SPD_10 = 0,
	PHY_AUX_SPD_100,
	PHY_AUX_SPD_1000,
	PHY_AUX_SPD_2500,
};

static int mt7988_2p5ge_phy_config_init(struct phy_device *phydev)
{
	int ret, i;
	const struct firmware *fw;
	struct device *dev = &phydev->mdio.dev;
	struct device_node *np;
	void __iomem *pmb_addr;
	void __iomem *md32_en_cfg_base;
	struct mtk_i2p5ge_phy_priv *priv = phydev->priv;
	u16 reg;
	struct pinctrl *pinctrl;

	if (!priv->fw_loaded) {
		np = of_find_compatible_node(NULL, NULL, "mediatek,2p5gphy-fw");
		if (!np)
			return -ENOENT;
		pmb_addr = of_iomap(np, 0);
		if (!pmb_addr)
			return -ENOMEM;
		md32_en_cfg_base = of_iomap(np, 1);
		if (!md32_en_cfg_base)
			return -ENOMEM;

		ret = request_firmware(&fw, MT7988_2P5GE_PMB, dev);
		if (ret) {
			dev_err(dev, "failed to load firmware: %s, ret: %d\n",
				MT7988_2P5GE_PMB, ret);
			return ret;
		}

		reg = readw(md32_en_cfg_base);
		if (reg & MD32_EN) {
			phy_set_bits(phydev, 0, BIT(15));
			usleep_range(10000, 11000);
		}
		phy_set_bits(phydev, 0, BIT(11));

		/* Write magic number to safely stall MCU */
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x800e, 0x1100);
		phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x800f, 0x00df);

		for (i = 0; i < fw->size - 1; i += 4)
			writel(*((uint32_t *)(fw->data + i)), pmb_addr + i);
		release_firmware(fw);

		writew(reg & ~MD32_EN, md32_en_cfg_base);
		writew(reg | MD32_EN, md32_en_cfg_base);
		phy_set_bits(phydev, 0, BIT(15));
		/* We need a delay here to stabilize initialization of MCU */
		usleep_range(7000, 8000);
		dev_info(dev, "Firmware loading/trigger ok.\n");

		priv->fw_loaded = true;
	}

	/* Setup LED */
	phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, MTK_PHY_LED0_ON_CTRL,
			 MTK_PHY_LED_ON_POLARITY | MTK_PHY_LED_ON_LINK10 |
			 MTK_PHY_LED_ON_LINK100 | MTK_PHY_LED_ON_LINK1000 |
			 MTK_PHY_LED_ON_LINK2500);
	phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, MTK_PHY_LED1_ON_CTRL,
			 MTK_PHY_LED_ON_FDX | MTK_PHY_LED_ON_HDX);

	pinctrl = devm_pinctrl_get_select(&phydev->mdio.dev, "i2p5gbe-led");
	if (IS_ERR(pinctrl)) {
		dev_err(&phydev->mdio.dev, "Fail to set LED pins!\n");
		return PTR_ERR(pinctrl);
	}

	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_LPI_PCS_DSP_CTRL,
		       MTK_PHY_LPI_SIG_EN_LO_THRESH100_MASK, 0);

	/* Enable 16-bit next page exchange bit if 1000-BT isn't advertizing */
	tr_modify(phydev, 0x0, 0xf, 0x3c, AUTO_NP_10XEN,
		  FIELD_PREP(AUTO_NP_10XEN, 0x1));

	/* Enable HW auto downshift */
	phy_modify_paged(phydev, MTK_PHY_PAGE_EXTENDED_1, MTK_PHY_AUX_CTRL_AND_STATUS,
			 0, MTK_PHY_ENABLE_DOWNSHIFT);

	return 0;
}

static int mt7988_2p5ge_phy_config_aneg(struct phy_device *phydev)
{
	bool changed = false;
	u32 adv;
	int ret;

	if (phydev->autoneg == AUTONEG_DISABLE) {
		/* Configure half duplex with genphy_setup_forced,
		 * because genphy_c45_pma_setup_forced does not support.
		 */
		return phydev->duplex != DUPLEX_FULL
			? genphy_setup_forced(phydev)
			: genphy_c45_pma_setup_forced(phydev);
	}

	ret = genphy_c45_an_config_aneg(phydev);
	if (ret < 0)
		return ret;
	if (ret > 0)
		changed = true;

	adv = linkmode_adv_to_mii_ctrl1000_t(phydev->advertising);
	ret = phy_modify_changed(phydev, MII_CTRL1000,
				 ADVERTISE_1000FULL | ADVERTISE_1000HALF,
				 adv);
	if (ret < 0)
		return ret;
	if (ret > 0)
		changed = true;

	return genphy_c45_check_and_restart_aneg(phydev, changed);
}

static int mt7988_2p5ge_phy_get_features(struct phy_device *phydev)
{
	int ret;

	ret = genphy_read_abilities(phydev);
	if (ret)
		return ret;

	/* We don't support HDX at MAC layer on mt7988.
	 * So mask phy's HDX capabilities, too.
	 */
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT,
			 phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT,
			 phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
			 phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
			 phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, phydev->supported);

	return 0;
}

static int mt7988_2p5ge_phy_read_status(struct phy_device *phydev)
{
	int ret;

	ret = genphy_update_link(phydev);
	if (ret)
		return ret;

	phydev->speed = SPEED_UNKNOWN;
	phydev->duplex = DUPLEX_UNKNOWN;
	phydev->pause = 0;
	phydev->asym_pause = 0;

	if (phydev->autoneg == AUTONEG_ENABLE) {
		if (phydev->autoneg_complete) {
			ret = genphy_c45_read_lpa(phydev);
			if (ret < 0)
				return ret;

			/* Read the link partner's 1G advertisement */
			ret = phy_read(phydev, MII_STAT1000);
			if (ret < 0)
				return ret;
			mii_stat1000_mod_linkmode_lpa_t(phydev->lp_advertising, ret);
		} else if (!linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
					      phydev->advertising) &&
			   linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
					     phydev->advertising)) {
			extend_an_new_lp_cnt_limit(phydev);
		}
	} else if (phydev->autoneg == AUTONEG_DISABLE) {
		linkmode_zero(phydev->lp_advertising);
	}

	ret = phy_read(phydev, PHY_AUX_CTRL_STATUS);
	if (ret < 0)
		return ret;

	switch (FIELD_GET(PHY_AUX_SPEED_MASK, ret)) {
	case PHY_AUX_SPD_10:
		phydev->speed = SPEED_10;
		break;
	case PHY_AUX_SPD_100:
		phydev->speed = SPEED_100;
		break;
	case PHY_AUX_SPD_1000:
		phydev->speed = SPEED_1000;
		break;
	case PHY_AUX_SPD_2500:
		phydev->speed = SPEED_2500;
		break;
	}

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_LINK_STATUS_MISC);
	if (ret < 0)
		return ret;
	phydev->duplex = (ret & MTK_PHY_FDX_ENABLE) ? DUPLEX_FULL : DUPLEX_HALF;
	/* FIXME: The current firmware always enables rate adaptation mode. */
	phydev->rate_matching = RATE_MATCH_PAUSE;

	return 0;
}

static int mt7988_2p5ge_phy_get_rate_matching(struct phy_device *phydev,
					      phy_interface_t iface)
{
	if (iface == PHY_INTERFACE_MODE_XGMII)
		return RATE_MATCH_PAUSE;
	return RATE_MATCH_NONE;
}

static const unsigned long supported_triggers = (BIT(TRIGGER_NETDEV_FULL_DUPLEX) |
						 BIT(TRIGGER_NETDEV_LINK)        |
						 BIT(TRIGGER_NETDEV_LINK_10)     |
						 BIT(TRIGGER_NETDEV_LINK_100)    |
						 BIT(TRIGGER_NETDEV_LINK_1000)   |
						 BIT(TRIGGER_NETDEV_LINK_2500)   |
						 BIT(TRIGGER_NETDEV_RX)          |
						 BIT(TRIGGER_NETDEV_TX));

static int mt798x_2p5ge_phy_led_blink_set(struct phy_device *phydev, u8 index,
					  unsigned long *delay_on,
					  unsigned long *delay_off)
{
	bool blinking = false;
	int err = 0;
	struct mtk_i2p5ge_phy_priv *priv = phydev->priv;

	if (index > 1)
		return -EINVAL;

	if (delay_on && delay_off && (*delay_on > 0) && (*delay_off > 0)) {
		blinking = true;
		*delay_on = 50;
		*delay_off = 50;
	}

	err = mtk_phy_hw_led_blink_set(phydev, index, &priv->led_state, blinking);
	if (err)
		return err;

	return mtk_phy_hw_led_on_set(phydev, index, &priv->led_state, false);
}

static int mt798x_2p5ge_phy_led_brightness_set(struct phy_device *phydev,
					       u8 index, enum led_brightness value)
{
	int err;
	struct mtk_i2p5ge_phy_priv *priv = phydev->priv;

	err = mtk_phy_hw_led_blink_set(phydev, index, &priv->led_state, false);
	if (err)
		return err;

	return mtk_phy_hw_led_on_set(phydev, index, &priv->led_state, (value != LED_OFF));
}

static int mt798x_2p5ge_phy_led_hw_is_supported(struct phy_device *phydev, u8 index,
						unsigned long rules)
{
	return mtk_phy_led_hw_is_supported(phydev, index, rules, supported_triggers);
}

static int mt798x_2p5ge_phy_led_hw_control_get(struct phy_device *phydev, u8 index,
					       unsigned long *rules)
{
	struct mtk_i2p5ge_phy_priv *priv = phydev->priv;

	return mtk_phy_led_hw_ctrl_get(phydev, index, rules, &priv->led_state,
				       MTK_2P5GPHY_LED_ON_SET,
				       MTK_2P5GPHY_LED_RX_BLINK_SET,
				       MTK_2P5GPHY_LED_TX_BLINK_SET);
};

static int mt798x_2p5ge_phy_led_hw_control_set(struct phy_device *phydev, u8 index,
					       unsigned long rules)
{
	struct mtk_i2p5ge_phy_priv *priv = phydev->priv;

	return mtk_phy_led_hw_ctrl_set(phydev, index, rules, &priv->led_state,
				       MTK_2P5GPHY_LED_ON_SET,
				       MTK_2P5GPHY_LED_RX_BLINK_SET,
				       MTK_2P5GPHY_LED_TX_BLINK_SET);
};

static void mt798x_phy_leds_state_init(struct phy_device *phydev)
{
	int i;

	for (i = 0; i < 2; ++i)
		mt798x_2p5ge_phy_led_hw_control_get(phydev, i, NULL);
}

static int mt7988_2p5ge_phy_probe(struct phy_device *phydev)
{
	struct mtk_i2p5ge_phy_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev,
			    sizeof(struct mtk_i2p5ge_phy_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->fw_loaded = false;
	phydev->priv = priv;

	mt798x_phy_leds_state_init(phydev);

	return 0;
}

static struct phy_driver mtk_gephy_driver[] = {
	{
		PHY_ID_MATCH_MODEL(0x00339c11),
		.name		= "MediaTek MT798x 2.5GbE PHY",
		.probe		= mt7988_2p5ge_phy_probe,
		.config_init	= mt7988_2p5ge_phy_config_init,
		.config_aneg    = mt7988_2p5ge_phy_config_aneg,
		.get_features	= mt7988_2p5ge_phy_get_features,
		.read_status	= mt7988_2p5ge_phy_read_status,
		.get_rate_matching	= mt7988_2p5ge_phy_get_rate_matching,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
		.led_blink_set	= mt798x_2p5ge_phy_led_blink_set,
		.led_brightness_set = mt798x_2p5ge_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_2p5ge_phy_led_hw_is_supported,
		.led_hw_control_get = mt798x_2p5ge_phy_led_hw_control_get,
		.led_hw_control_set = mt798x_2p5ge_phy_led_hw_control_set,
	},
};

module_phy_driver(mtk_gephy_driver);

static struct mdio_device_id __maybe_unused mtk_2p5ge_phy_tbl[] = {
	{ PHY_ID_MATCH_VENDOR(0x00339c00) },
	{ }
};

MODULE_DESCRIPTION("MediaTek 2.5Gb Ethernet PHY driver");
MODULE_AUTHOR("SkyLake Huang <SkyLake.Huang@mediatek.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, mtk_2p5ge_phy_tbl);
