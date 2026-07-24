// SPDX-License-Identifier: GPL-2.0+
#include <linux/pinctrl/consumer.h>
#include <linux/phy.h>

#include "mtk.h"
#include "mtk-ge-soc.h"

#define AIROHA_PHY_MAX_LEDS			2

static int an7581_phy_probe(struct phy_device *phydev)
{
	struct mtk_socphy_priv *priv;
	struct pinctrl *pinctrl;

	/* Toggle pinctrl to enable PHY LED */
	pinctrl = devm_pinctrl_get_select(&phydev->mdio.dev, "gbe-led");
	if (IS_ERR(pinctrl))
		dev_err(&phydev->mdio.bus->dev,
			"Failed to setup PHY LED pinctrl\n");

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	return 0;
}

static int an7581_phy_led_polarity_set(struct phy_device *phydev, int index,
				       unsigned long modes)
{
	u16 val = 0;
	u32 mode;

	if (index >= AIROHA_PHY_MAX_LEDS)
		return -EINVAL;

	for_each_set_bit(mode, &modes, __PHY_LED_MODES_NUM) {
		switch (mode) {
		case PHY_LED_ACTIVE_LOW:
			val = MTK_PHY_LED_ON_POLARITY;
			break;
		case PHY_LED_ACTIVE_HIGH:
			break;
		default:
			return -EINVAL;
		}
	}

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, index ?
			      MTK_PHY_LED1_ON_CTRL : MTK_PHY_LED0_ON_CTRL,
			      MTK_PHY_LED_ON_POLARITY, val);
}

static int an7583_phy_config_init(struct phy_device *phydev)
{
	/* BMCR_PDOWN is enabled by default */
	return phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);
}

static struct phy_driver mtk_socphy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7581),
		.name		= "Airoha AN7581 PHY",
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
	},
	{
		PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7583),
		.name		= "Airoha AN7583 PHY",
		.config_init	= an7583_phy_config_init,
		.probe		= an7581_phy_probe,
		.led_blink_set	= mt798x_phy_led_blink_set,
		.led_brightness_set = mt798x_phy_led_brightness_set,
		.led_hw_is_supported = mt798x_phy_led_hw_is_supported,
		.led_hw_control_set = mt798x_phy_led_hw_control_set,
		.led_hw_control_get = mt798x_phy_led_hw_control_get,
		.led_polarity_set = an7581_phy_led_polarity_set,
	},
};

module_phy_driver(mtk_socphy_driver);

static const struct mdio_device_id __maybe_unused airoha_socphy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7581) },
	{ PHY_ID_MATCH_EXACT(AIROHA_GPHY_ID_AN7583) },
	{ }
};

MODULE_DESCRIPTION("Airoha SoC Gigabit Ethernet PHY driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, airoha_socphy_tbl);
