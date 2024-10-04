// SPDX-License-Identifier: GPL-2.0+
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/phy.h>

#include "mtk.h"

#define MTK_GPHY_ID_MT7530		0x03a29412
#define MTK_GPHY_ID_MT7531		0x03a29441

#define MTK_EXT_PAGE_ACCESS		0x1f
#define MTK_PHY_PAGE_STANDARD		0x0000
#define MTK_PHY_PAGE_EXTENDED		0x0001
#define MTK_PHY_PAGE_EXTENDED_2		0x0002
#define MTK_PHY_PAGE_EXTENDED_3		0x0003
#define MTK_PHY_PAGE_EXTENDED_2A30	0x2a30
#define MTK_PHY_PAGE_EXTENDED_52B5	0x52b5

struct mtk_gephy_priv {
	unsigned long		led_state;
};

static void mtk_gephy_config_init(struct phy_device *phydev)
{
	/* Enable HW auto downshift */
	phy_modify_paged(phydev, MTK_PHY_PAGE_EXTENDED, 0x14, 0, BIT(4));

	/* Increase SlvDPSready time */
	phy_select_page(phydev, MTK_PHY_PAGE_EXTENDED_52B5);
	__phy_write(phydev, 0x10, 0xafae);
	__phy_write(phydev, 0x12, 0x2f);
	__phy_write(phydev, 0x10, 0x8fae);
	phy_restore_page(phydev, MTK_PHY_PAGE_STANDARD, 0);

	/* Adjust 100_mse_threshold */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x123, 0xffff);

	/* Disable mcc */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xa6, 0x300);
}

static int mt7530_phy_config_init(struct phy_device *phydev)
{
	mtk_gephy_config_init(phydev);

	/* Increase post_update_timer */
	phy_write_paged(phydev, MTK_PHY_PAGE_EXTENDED_3, 0x11, 0x4b);

	return 0;
}

static int mt7531_phy_config_init(struct phy_device *phydev)
{
	mtk_gephy_config_init(phydev);

	/* PHY link down power saving enable */
	phy_set_bits(phydev, 0x17, BIT(4));
	phy_clear_bits_mmd(phydev, MDIO_MMD_VEND1, 0xc6, 0x300);

	/* Set TX Pair delay selection */
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x13, 0x404);
	phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x14, 0x404);

	return 0;
}

static int mt7531_phy_probe(struct phy_device *phydev)
{
	struct mtk_gephy_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(struct mtk_gephy_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	mtk_phy_leds_state_init(phydev);

	return 0;
}

static int mt753x_phy_led_blink_set(struct phy_device *phydev, u8 index,
				    unsigned long *delay_on,
				    unsigned long *delay_off)
{
	struct mtk_gephy_priv *priv = phydev->priv;
	bool blinking = false;
	int err = 0;

	err = mtk_phy_led_num_dly_cfg(index, delay_on, delay_off, &blinking);
	if (err < 0)
		return err;

	err = mtk_phy_hw_led_blink_set(phydev, index, &priv->led_state,
				       blinking);
	if (err)
		return err;

	return mtk_phy_hw_led_on_set(phydev, index, &priv->led_state,
				     MTK_GPHY_LED_ON_MASK, false);
}

static int mt753x_phy_led_brightness_set(struct phy_device *phydev,
					 u8 index, enum led_brightness value)
{
	struct mtk_gephy_priv *priv = phydev->priv;
	int err;

	err = mtk_phy_hw_led_blink_set(phydev, index, &priv->led_state, false);
	if (err)
		return err;

	return mtk_phy_hw_led_on_set(phydev, index, &priv->led_state,
				     MTK_GPHY_LED_ON_MASK, (value != LED_OFF));
}

static const unsigned long supported_triggers =
	BIT(TRIGGER_NETDEV_FULL_DUPLEX) |
	BIT(TRIGGER_NETDEV_HALF_DUPLEX) |
	BIT(TRIGGER_NETDEV_LINK)        |
	BIT(TRIGGER_NETDEV_LINK_10)     |
	BIT(TRIGGER_NETDEV_LINK_100)    |
	BIT(TRIGGER_NETDEV_LINK_1000)   |
	BIT(TRIGGER_NETDEV_RX)          |
	BIT(TRIGGER_NETDEV_TX);

static int mt753x_phy_led_hw_is_supported(struct phy_device *phydev, u8 index,
					  unsigned long rules)
{
	return mtk_phy_led_hw_is_supported(phydev, index, rules,
					   supported_triggers);
}

static int mt753x_phy_led_hw_control_get(struct phy_device *phydev, u8 index,
					 unsigned long *rules)
{
	struct mtk_gephy_priv *priv = phydev->priv;

	return mtk_phy_led_hw_ctrl_get(phydev, index, rules, &priv->led_state,
				       MTK_GPHY_LED_ON_SET,
				       MTK_GPHY_LED_RX_BLINK_SET,
				       MTK_GPHY_LED_TX_BLINK_SET);
};

static int mt753x_phy_led_hw_control_set(struct phy_device *phydev, u8 index,
					 unsigned long rules)
{
	struct mtk_gephy_priv *priv = phydev->priv;

	return mtk_phy_led_hw_ctrl_set(phydev, index, rules, &priv->led_state,
				       MTK_GPHY_LED_ON_SET,
				       MTK_GPHY_LED_RX_BLINK_SET,
				       MTK_GPHY_LED_TX_BLINK_SET);
};

static struct phy_driver mtk_gephy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7530),
		.name		= "MediaTek MT7530 PHY",
		.config_init	= mt7530_phy_config_init,
		/* Interrupts are handled by the switch, not the PHY
		 * itself.
		 */
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
	{
		PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7531),
		.name		= "MediaTek MT7531 PHY",
		.probe		= mt7531_phy_probe,
		.config_init	= mt7531_phy_config_init,
		/* Interrupts are handled by the switch, not the PHY
		 * itself.
		 */
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
		.led_blink_set	= mt753x_phy_led_blink_set,
		.led_brightness_set = mt753x_phy_led_brightness_set,
		.led_hw_is_supported = mt753x_phy_led_hw_is_supported,
		.led_hw_control_set = mt753x_phy_led_hw_control_set,
		.led_hw_control_get = mt753x_phy_led_hw_control_get,
	},
};

module_phy_driver(mtk_gephy_driver);

static struct mdio_device_id __maybe_unused mtk_gephy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7530) },
	{ PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7531) },
	{ }
};

MODULE_DESCRIPTION("MediaTek Gigabit Ethernet PHY driver");
MODULE_AUTHOR("DENG, Qingfang <dqfext@gmail.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, mtk_gephy_tbl);
