// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for the Airoha AN8801 Gigabit PHY.
 *
 * Copyright (C) 2025 Airoha Technology Corp.
 * Copyright (C) 2025 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>

#define AN8801R_PHY_ID			0xc0ff0421

/* MII Registers */
#define AIR_EXT_PAGE_ACCESS		0x1f
#define   AIR_PHY_PAGE_STANDARD		0
#define   AIR_PHY_PAGE_EXTENDED_1	1
#define   AIR_PHY_PAGE_EXTENDED_4	4

/* MII Registers - Airoha Page 1 */
#define AN8801_EXT_REG_PHY		0x14
#define   AN8801_EXT_PHY_STATUS0	GENMASK(1, 0)
#define   AN8801_EXT_PHY_DOWNSHIFT_CTL	GENMASK(3, 2) /* 2 to 5 1G auto-neg attempts (0..3) */
#define   AN8801_EXT_PHY_DOWNSHIFT_EN	BIT(4)
#define   AN8801_EXT_PHY_CTRL0		BIT(5)
#define   AN8801_EXT_PHY_STATUS1	GENMASK(8, 6)
#define   AN8801_EXT_PHY_CTRL1		GENMASK(14, 9)

/* MII Registers - Airoha Page 4 */
#define AN8801_PBUS_ACCESS		BIT(28)
#define AN8801_PBUS_EPHY_ACCESS		BIT(24)
#define AN8801_PBUS_CL22_ACCESS		BIT(23)

#define AIR_BPBUS_MODE			0x10
#define AIR_BPBUS_WR_ADDR_HIGH		0x11
#define AIR_BPBUS_WR_ADDR_LOW		0x12
#define AIR_BPBUS_WR_DATA_HIGH		0x13
#define AIR_BPBUS_WR_DATA_LOW		0x14
#define AIR_BPBUS_RD_ADDR_HIGH		0x15
#define AIR_BPBUS_RD_ADDR_LOW		0x16
#define AIR_BPBUS_RD_DATA_HIGH		0x17
#define AIR_BPBUS_RD_DATA_LOW		0x18

/* BPBUS Registers */
#define AN8801_BPBUS_REG_LED_GPIO	0x54
#define AN8801_BPBUS_REG_LED_ID_SEL	0x58
#define   LED_ID_GPIO_SEL(led, gpio)	((led) << ((gpio) * 3))
#define AN8801_BPBUS_REG_GPIO_MODE	0x70
#define AN8801_BPBUS_REG_PHY_IRQ_GPIO	0x7c
#define   AN8801_PHY_IRQ_GPIO_NUM_MASK	GENMASK(19, 16)
#define   AN8801_PHY_IRQ_GPIO_NUM	1

#define AN8801_BPBUS_REG_CKO		0x1a4
#define AN8801_CKO_OUTPUT_MODE_AUTO	3

#define AN8801_BPBUS_REG_LINK_MODE	0x5054
#define  AN8801_BPBUS_LINK_MODE_1000	BIT(0)

#define AN8801_BPBUS_REG_BYPASS_PTP	0x21c004
#define   AN8801_BYP_PTP_SGMII_TO_GPHY	BIT(8)
#define   AN8801_BYP_PTP_RGMII_TO_GPHY	BIT(0)

#define AN8801_BPBUS_REG_TXDLY_STEP	0x21c024
#define   RGMII_DELAY_STEP_MASK		GENMASK(2, 0)
#define   RGMII_TXDELAY_FORCE_MODE	BIT(24)

#define AN8801_BPBUS_REG_RXDLY_STEP	0x21c02c
#define   RGMII_RXDELAY_ALIGN		BIT(4)
#define   RGMII_RXDELAY_FORCE_MODE	BIT(24)

#define AN8801_BPBUS_REG_EFIFO_CTL(x)	(0x270004 + (0x100 * (x))) /* 0..2 */
#define   AN8801_EFIFO_ALL_EN		GENMASK(7, 0)
#define   AN8801_EFIFO_RX_EN		BIT(0)
#define   AN8801_EFIFO_TX_EN		BIT(1)
#define   AN8801_EFIFO_RX_CLK_EN	BIT(2)
#define   AN8801_EFIFO_TX_CLK_EN	BIT(3)
#define   AN8801_EFIFO_RX_EEE_EN	BIT(4)
#define   AN8801_EFIFO_TX_EEE_EN	BIT(5)
#define   AN8801_EFIFO_RX_ODD_NIBBLE_EN	BIT(6)
#define   AN8801_EFIFO_TX_ODD_NIBBLE_EN	BIT(7)

#define AN8801_BPBUS_REG_WOL_MAC_16_47	0x285114
#define AN8801_BPBUS_REG_WOL_MAC_0_15	0x285118

#define AN8801_BPBUS_REG_WAKEUP_CTL1	0x285400
#define   AN8801_WOL_WAKE_MAGIC_EN	GENMASK(3, 1)

#define AN8801_BPBUS_REG_WAKEUP_CTL2	0x285404
#define   AN8801_WAKE_OUT_TYPE_PULSE	BIT(0) /* Set/Unset: Pulse/Static */
#define   AN8801_WAKE_OUT_POLARITY_NEG	BIT(1) /* Set/Unset: Negative/Positive */
#define   AN8801_WAKE_OUT_WIDTH		GENMASK(2, 3)
#define    AN8801_WAKE_OUT_84MS		0
#define    AN8801_WAKE_OUT_168MS	1
#define    AN8801_WAKE_OUT_336MS	2
#define    AN8801_WAKE_OUT_672MS	3
#define   AN8801_WAKE_OUT_EN		BIT(4)
#define   AN8801_PME_WAKEUP_CLR		BIT(8)

#define AN8801_BPBUS_REG_WAKE_IRQ_EN	0x285700
#define AN8801_BPBUS_REG_WAKE_IRQ_STS	0x285704
#define   AN8801_IRQ_WAKE_LNKCHG	BIT(0) /* Wake on link change */
#define   AN8801_IRQ_WAKE_UNIPKT	BIT(1) /* Wake on unicast packet */
#define   AN8801_IRQ_WAKE_MULPKT	BIT(2) /* Wake on multicast packet */
#define   AN8801_IRQ_WAKE_BCPKT		BIT(3) /* Wake on broadcast packet */
#define   AN8801_IRQ_WAKE_MAGICPKT	BIT(4) /* Wake on magic packet */
#define   AN8801_IRQ_WAKE_ALL		GENMASK(4, 0)

/* MDIO_MMD_VEND1 Registers */
#define AN8801_PHY_TX_PAIR_DLY_SEL_GBE	0x13
#define   AN8801_PHY_PAIR_DLY_SEL_A_GBE	GENMASK(14, 12)
#define   AN8801_PHY_PAIR_DLY_SEL_B_GBE	GENMASK(10, 8)
#define   AN8801_PHY_PAIR_DLY_SEL_C_GBE	GENMASK(6, 4)
#define   AN8801_PHY_PAIR_DLY_SEL_D_GBE	GENMASK(2, 0)
#define AN8801_PHY_RXADC_CTRL		0xd8
#define   AN8801_PHY_RXADC_SAMP_PHSEL_A	BIT(12)
#define   AN8801_PHY_RXADC_SAMP_PHSEL_B	BIT(8)
#define   AN8801_PHY_RXADC_SAMP_PHSEL_C	BIT(4)
#define   AN8801_PHY_RXADC_SAMP_PHSEL_D	BIT(0)
#define AN8801_PHY_RXADC_REV_0		0xd9
#define   AN8801_PHY_RXADC_REV_MASK_A	GENMASK(15, 8)
#define   AN8801_PHY_RXADC_REV_MASK_B	GENMASK(7, 0)
#define AN8801_PHY_RXADC_REV_1		0xda
#define   AN8801_PHY_RXADC_REV_MASK_C	GENMASK(15, 8)
#define   AN8801_PHY_RXADC_REV_MASK_D	GENMASK(7, 0)

/* MDIO_MMD_VEND2 Registers */
#define LED_BCR				0x21
#define   LED_BCR_MODE_MASK		GENMASK(1, 0)
#define   LED_BCR_TIME_TEST		BIT(2)
#define   LED_BCR_CLK_EN		BIT(3)
#define   LED_BCR_EVT_ALL		BIT(4)
#define   LED_BCR_EXT_CTRL		BIT(15)
#define   LED_BCR_MODE_DISABLE		0
#define   LED_BCR_MODE_2LED		1
#define   LED_BCR_MODE_3LED_1		2
#define   LED_BCR_MODE_3LED_2		3

#define LED_ON_DUR			0x22
#define   LED_ON_DUR_MASK		GENMASK(15, 0)

#define LED_BLINK_DUR			0x23
#define   LED_BLINK_DUR_MASK		GENMASK(15, 0)

#define LED_ON_CTRL(i)			(0x24 + ((i) * 2))
#define   LED_ON_EVT_MASK		GENMASK(6, 0)
#define   LED_ON_EVT_LINK_1000M		BIT(0)
#define   LED_ON_EVT_LINK_100M		BIT(1)
#define   LED_ON_EVT_LINK_10M		BIT(2)
#define   LED_ON_EVT_LINK_DN		BIT(3)
#define   LED_ON_EVT_FDX		BIT(4)
#define   LED_ON_EVT_HDX		BIT(5)
#define   LED_ON_EVT_FORCE		BIT(6)
#define   LED_ON_POL			BIT(14)
#define   LED_ON_EN			BIT(15)

#define LED_BLINK_CTRL(i)		(0x25 + ((i) * 2))
#define LED_BLINK_EVT_MASK		GENMASK(9, 0)
#define LED_BLINK_EVT_1000M_TX		BIT(0)
#define LED_BLINK_EVT_1000M_RX		BIT(1)
#define LED_BLINK_EVT_100M_TX		BIT(2)
#define LED_BLINK_EVT_100M_RX		BIT(3)
#define LED_BLINK_EVT_10M_TX		BIT(4)
#define LED_BLINK_EVT_10M_RX		BIT(5)
#define LED_BLINK_EVT_COLLISION		BIT(6)
#define LED_BLINK_EVT_RX_CRC_ERR	BIT(7)
#define LED_BLINK_EVT_RX_IDLE_ERR	BIT(8)
#define LED_BLINK_EVT_FORCE		BIT(9)

#define AN8801R_NUM_LEDS		3
#define AN8801_PERIOD_SHIFT		15
#define AN8801_PERIOD_UNIT		32768 /* (1 << AN8801_PERIOD_SHIFT) */
#define AN8801_MAX_PERIOD_MS		2147

#define LED_BLINK_DURATION_UNIT		780
#define LED_BLINK_DURATION(f)		(LED_BLINK_DURATION_UNIT << (f))

#define AN8801_LED_DURATION_UNIT_US	32768

#define AN8801_REG_PHY_INTERNAL0	0x600
#define AN8801_REG_PHY_INTERNAL1	0x601
#define   AN8801_PHY_INTFUNC_MASK	GENMASK(15, 0) /* PHY internal functions */

enum an8801r_led_fn {
	AN8801R_LED_FN_NONE,
	AN8801R_LED_FN_LINK,
	AN8801R_LED_FN_ACTIVITY,
	AN8801R_LED_FN_MAX,
};

static int an8801r_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, AIR_EXT_PAGE_ACCESS);
}

static int an8801r_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, AIR_EXT_PAGE_ACCESS, page);
}

static int __air_buckpbus_reg_write(struct phy_device *phydev,
				    u32 addr, u32 data)
{
	int ret;

	addr |= AN8801_PBUS_ACCESS;

	ret = __phy_write(phydev, AIR_BPBUS_MODE, MII_MMD_CTRL_ADDR);
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_WR_ADDR_HIGH, upper_16_bits(addr));
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_WR_ADDR_LOW, lower_16_bits(addr));
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_WR_DATA_HIGH, upper_16_bits(data));
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_WR_DATA_LOW, lower_16_bits(data));
	if (ret < 0)
		return ret;

	return 0;
}

static int __air_buckpbus_reg_read(struct phy_device *phydev,
				   u32 addr, u32 *data)
{
	int pbus_data_l, pbus_data_h;
	int ret;

	addr |= AN8801_PBUS_ACCESS;

	ret = __phy_write(phydev, AIR_BPBUS_MODE, MII_MMD_CTRL_ADDR);
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_RD_ADDR_HIGH, upper_16_bits(addr));
	if (ret < 0)
		return ret;

	ret = __phy_write(phydev, AIR_BPBUS_RD_ADDR_LOW, lower_16_bits(addr));
	if (ret < 0)
		return ret;

	ret = __phy_read(phydev, AIR_BPBUS_RD_DATA_HIGH);
	if (pbus_data_h < 0)
		return pbus_data_h;

	pbus_data_l = __phy_read(phydev, AIR_BPBUS_RD_DATA_LOW);
	if (pbus_data_l < 0)
		return pbus_data_l;

	*data = (pbus_data_h << 16) | pbus_data_l;
	return 0;
}

static int air_buckpbus_reg_rmw(struct phy_device *phydev,
				u32 addr, u32 mask, u32 set)
{
	u32 data_old, data_new;
	int prev_page, ret;

	prev_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);
	if (prev_page < 0)
		return prev_page;

	ret = __air_buckpbus_reg_read(phydev, addr, &data_old);
	if (ret)
		return phy_restore_page(phydev, prev_page, ret);

	data_new = data_old & ~mask;
	data_new |= set;
	if (data_new != data_old)
		ret = __air_buckpbus_reg_write(phydev, addr, data_new);

	return phy_restore_page(phydev, prev_page, ret);
}

static int air_buckpbus_reg_set_bits(struct phy_device *phydev,
				     u32 addr, u32 mask)
{
	return air_buckpbus_reg_rmw(phydev, addr, mask, mask);
}

static int air_buckpbus_reg_clear_bits(struct phy_device *phydev,
				       u32 addr, u32 mask)
{
	return air_buckpbus_reg_rmw(phydev, addr, mask, 0);
}

static int air_buckpbus_reg_write(struct phy_device *phydev, u32 addr, u32 data)
{
	int prev_page, ret = 0;

	prev_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);
	if (prev_page < 0)
		return prev_page;

	ret = __air_buckpbus_reg_write(phydev, addr, data);

	return phy_restore_page(phydev, prev_page, ret);
}

static int air_buckpbus_reg_read(struct phy_device *phydev, u32 addr, u32 *data)
{
	int prev_page, ret;

	prev_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_4);
	if (prev_page < 0)
		return prev_page;

	ret = __air_buckpbus_reg_read(phydev, addr, data);

	return phy_restore_page(phydev, prev_page, ret);
}

static u32 an8801r_led_blink_ms_to_hw(unsigned long req_ms)
{
	u32 req_ns, regval;

	if (req_ms > AN8801_MAX_PERIOD_MS)
		req_ms = AN8801_MAX_PERIOD_MS;

	req_ns = req_ms * 1000000;

	/* Round to the nearest period unit... */
	regval = req_ns + (AN8801_PERIOD_UNIT / 2);

	/* ...and now divide by the full period */
	regval >>= AN8801_PERIOD_SHIFT;

	return regval;
}

static int an8801r_led_blink_set(struct phy_device *phydev, u8 index,
				 unsigned long *delay_on,
				 unsigned long *delay_off)
{
	u32 hw_delay_on, hw_delay_off;
	bool blink;
	int ret;

	if (index >= AN8801R_NUM_LEDS)
		return -EINVAL;

	if (delay_on && delay_off) {
		blink = true;

		if (*delay_on == 0 || *delay_off == 0) {
			*delay_on = 64;
			*delay_off = 64;
		}

		hw_delay_on = an8801r_led_blink_ms_to_hw(*delay_on);
		hw_delay_off = an8801r_led_blink_ms_to_hw(*delay_off);
	} else {
		blink = false;
	}

	if (blink) {
		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_BLINK_DUR,
				    LED_BLINK_DURATION(hw_delay_on));
		if (ret)
			goto error;

		ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_ON_DUR,
				    LED_BLINK_DURATION(hw_delay_off) >> 1);
		if (ret)
			goto error;
	}

	ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			     LED_ON_EN, blink ? LED_ON_EN : 0);
	if (ret)
		return ret;

	return 0;
error:
	phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
		       LED_ON_EN, 0);
	return ret;
}

static int an8801r_led_brightness_set(struct phy_device *phydev, u8 index,
				      enum led_brightness value)
{
	int ret;

	if (index >= AN8801R_NUM_LEDS)
		return -EINVAL;

	ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			     LED_ON_EVT_MASK,
			     (value == LED_OFF) ? 0 : LED_ON_EVT_FORCE);
	if (ret)
		return ret;

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			      LED_ON_EN, (value == LED_OFF) ? 0 : LED_ON_EN);
}

static int an8801r_led_hw_control_get(struct phy_device *phydev, u8 index,
				      unsigned long *rules)
{
	int on, blink;

	on = phy_read_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index));
	if (on < 0)
		return on;

	blink = phy_read_mmd(phydev, MDIO_MMD_VEND2, LED_BLINK_CTRL(index));
	if (blink < 0)
		return blink;

	if (FIELD_GET(LED_ON_EVT_LINK_10M, on))
		__set_bit(TRIGGER_NETDEV_LINK_10, rules);

	if (FIELD_GET(LED_ON_EVT_LINK_100M, on))
		__set_bit(TRIGGER_NETDEV_LINK_100, rules);

	if (FIELD_GET(LED_ON_EVT_LINK_1000M, on))
		__set_bit(TRIGGER_NETDEV_LINK_1000, rules);

	if (FIELD_GET(LED_ON_EVT_LINK_10M, on) &&
	    FIELD_GET(LED_ON_EVT_LINK_100M, on) &&
	    FIELD_GET(LED_ON_EVT_LINK_1000M, on))
		__set_bit(TRIGGER_NETDEV_LINK, rules);

	if (FIELD_GET(LED_BLINK_EVT_10M_RX, blink) ||
	    FIELD_GET(LED_BLINK_EVT_100M_RX, blink) ||
	    FIELD_GET(LED_BLINK_EVT_1000M_RX, blink))
		__set_bit(TRIGGER_NETDEV_RX, rules);

	if (FIELD_GET(LED_BLINK_EVT_10M_TX, blink) ||
	    FIELD_GET(LED_BLINK_EVT_100M_TX, blink) ||
	    FIELD_GET(LED_BLINK_EVT_1000M_TX, blink))
		__set_bit(TRIGGER_NETDEV_TX, rules);

	if (FIELD_GET(LED_BLINK_EVT_RX_CRC_ERR, blink))
		__set_bit(TRIGGER_NETDEV_RX_ERR, rules);

	return 0;
}

static int an8801r_led_trig_to_hw(unsigned long rules, u16 *on, u16 *blink)
{
	if (test_bit(TRIGGER_NETDEV_LINK_10, &rules))
		*on |= LED_ON_EVT_LINK_10M;

	if (test_bit(TRIGGER_NETDEV_LINK_100, &rules))
		*on |= LED_ON_EVT_LINK_100M;

	if (test_bit(TRIGGER_NETDEV_LINK_1000, &rules))
		*on |= LED_ON_EVT_LINK_1000M;

	if (test_bit(TRIGGER_NETDEV_LINK, &rules)) {
		*on |= LED_ON_EVT_LINK_10M;
		*on |= LED_ON_EVT_LINK_100M;
		*on |= LED_ON_EVT_LINK_1000M;
	}

	if (test_bit(TRIGGER_NETDEV_RX, &rules)) {
		*blink |= LED_BLINK_EVT_10M_RX;
		*blink |= LED_BLINK_EVT_100M_RX;
		*blink |= LED_BLINK_EVT_1000M_RX;
	}

	if (test_bit(TRIGGER_NETDEV_TX, &rules)) {
		*blink |= LED_BLINK_EVT_10M_TX;
		*blink |= LED_BLINK_EVT_100M_TX;
		*blink |= LED_BLINK_EVT_1000M_TX;
	}

	if (test_bit(TRIGGER_NETDEV_RX_ERR, &rules))
		*blink |= LED_BLINK_EVT_RX_CRC_ERR;

	if (rules && !*on && !*blink)
		return -EOPNOTSUPP;

	return 0;
}

static int an8801r_led_hw_is_supported(struct phy_device *phydev, u8 index,
				       unsigned long rules)
{
	u16 on = 0, blink = 0;

	if (index >= AN8801R_NUM_LEDS)
		return -EINVAL;

	return an8801r_led_trig_to_hw(rules, &on, &blink);
}

static int an8801r_led_hw_control_set(struct phy_device *phydev, u8 index,
				      unsigned long rules)
{
	u16 on = 0, blink = 0;
	int ret;

	if (index >= AN8801R_NUM_LEDS)
		return -EINVAL;

	ret = an8801r_led_trig_to_hw(rules, &on, &blink);
	if (ret)
		return ret;

	ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			     LED_ON_EVT_MASK, on);
	if (ret)
		return ret;

	ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_BLINK_CTRL(index),
			     LED_BLINK_EVT_MASK, blink);

	if (ret)
		return ret;

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			      LED_ON_EN, on | blink ? LED_ON_EN : 0);
}

static int an8801r_led_polarity_set(struct phy_device *phydev, int index,
				    unsigned long modes)
{
	unsigned long mode;
	bool active_high;

	if (index >= AN8801R_NUM_LEDS)
		return -EINVAL;

	for_each_set_bit(mode, &modes, __PHY_LED_MODES_NUM) {
		switch (mode) {
		case PHY_LED_ACTIVE_HIGH:
			active_high = true;
			break;
		case PHY_LED_ACTIVE_LOW:
			active_high = false;
			break;
		default:
			return -EINVAL;
		}
	}

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(index),
			      LED_ON_POL, active_high ? LED_ON_POL : 0);
}

static int an8801r_led_init(struct phy_device *phydev, u8 *led_cfg)
{
	int led_id, ret;

	/* Set LED BCR Enable */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, LED_BCR,
			       LED_BCR_EXT_CTRL | LED_BCR_CLK_EN);
	if (ret)
		return ret;

	for (led_id = 0; led_id < AN8801R_NUM_LEDS; led_id++) {
		unsigned long led_trigger = 0;
		u32 led_gpio = led_id + 1;

		switch (led_cfg[led_id]) {
		case AN8801R_LED_FN_LINK:
			led_trigger = BIT(TRIGGER_NETDEV_LINK);
			break;
		case AN8801R_LED_FN_ACTIVITY:
			led_trigger = BIT(TRIGGER_NETDEV_RX) |
				    BIT(TRIGGER_NETDEV_TX);
			break;
		default:
			led_trigger = 0;
			break;
		}

		ret = air_buckpbus_reg_set_bits(phydev,
						AN8801_BPBUS_REG_LED_GPIO,
						BIT(led_gpio));
		if (ret)
			return ret;

		ret = air_buckpbus_reg_set_bits(phydev,
						AN8801_BPBUS_REG_LED_ID_SEL,
						LED_ID_GPIO_SEL(led_id,
								led_gpio));
		if (ret)
			return ret;

		ret = air_buckpbus_reg_clear_bits(phydev,
						  AN8801_BPBUS_REG_GPIO_MODE,
						  BIT(led_gpio));
		if (ret)
			return ret;

		if (!led_trigger)
			continue;

		ret = an8801r_led_hw_control_set(phydev, led_id, led_trigger);
		if (ret)
			return ret;
	}
	return 0;
}

static int an8801r_ack_interrupt(struct phy_device *phydev)
{
	u32 reg_val;
	int ret;

	/* Reset WOL status */
	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_WAKEUP_CTL2,
				     AN8801_PME_WAKEUP_CLR |
				     AN8801_WAKE_OUT_POLARITY_NEG);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_read(phydev, AN8801_BPBUS_REG_WAKEUP_CTL1,
				    &reg_val);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_WAKEUP_CTL1, 0);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_WAKEUP_CTL1,
				     reg_val | BIT(4));
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_WAKEUP_CTL2,
				     AN8801_WAKE_OUT_POLARITY_NEG |
				     AN8801_WAKE_OUT_EN);
	if (ret)
		return ret;

	/* Clear the interrupts by writing the reg */
	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_WAKE_IRQ_STS,
				     AN8801_IRQ_WAKE_ALL);
	if (ret)
		return ret;

	return 0;
}

static int an8801r_config_intr(struct phy_device *phydev)
{
	int ret;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		u32 val = FIELD_PREP(AN8801_PHY_IRQ_GPIO_NUM_MASK,
				     AN8801_PHY_IRQ_GPIO_NUM);

		ret = air_buckpbus_reg_write(phydev,
					     AN8801_BPBUS_REG_PHY_IRQ_GPIO,
					     val);
		if (ret)
			return ret;

		ret = air_buckpbus_reg_set_bits(phydev,
						AN8801_BPBUS_REG_WAKE_IRQ_EN,
						AN8801_IRQ_WAKE_LNKCHG);
		if (ret)
			return ret;
	} else {
		ret = air_buckpbus_reg_write(phydev,
					     AN8801_BPBUS_REG_PHY_IRQ_GPIO, 0);
		if (ret)
			return ret;

		ret = air_buckpbus_reg_clear_bits(phydev,
						  AN8801_BPBUS_REG_WAKE_IRQ_EN,
						  AN8801_IRQ_WAKE_LNKCHG);
		if (ret)
			return ret;
	}

	return an8801r_ack_interrupt(phydev);
}

static int an8801r_did_interrupt(struct phy_device *phydev)
{
	u32 irq_en, irq_status;
	int ret;

	ret = air_buckpbus_reg_read(phydev, AN8801_BPBUS_REG_WAKE_IRQ_EN,
				    &irq_en);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_read(phydev, AN8801_BPBUS_REG_WAKE_IRQ_STS,
				    &irq_status);
	if (ret)
		return ret;

	if (irq_status & AN8801_IRQ_WAKE_MAGICPKT)
		return 0;

	if ((irq_en & AN8801_IRQ_WAKE_LNKCHG) &&
	    (irq_status & AN8801_IRQ_WAKE_LNKCHG))
		return 0;

	return -EINVAL;
}

static irqreturn_t an8801r_handle_interrupt(struct phy_device *phydev)
{
	int ret;

	ret = an8801r_did_interrupt(phydev);
	if (ret)
		return IRQ_NONE;

	ret = an8801r_ack_interrupt(phydev);
	if (ret)
		return IRQ_NONE;

	phy_trigger_machine(phydev);

	return IRQ_HANDLED;
}

static void an8801r_get_wol(struct phy_device *phydev,
			    struct ethtool_wolinfo *wol)
{
	u32 reg_val;

	air_buckpbus_reg_read(phydev, AN8801_BPBUS_REG_WAKEUP_CTL1, &reg_val);

	wol->supported = WAKE_MAGIC;

	if (reg_val & AN8801_WOL_WAKE_MAGIC_EN)
		wol->wolopts |= WAKE_MAGIC;
	else
		wol->wolopts &= ~WAKE_MAGIC;
}

static int an8801r_set_wol(struct phy_device *phydev,
			   struct ethtool_wolinfo *wol)
{
	struct net_device *attach_dev = phydev->attached_dev;
	const unsigned char *macaddr = attach_dev->dev_addr;
	u32 reg_val;
	int ret;

	if (wol->wolopts & WAKE_MAGIC) {
		/* MAC bits 16..47 */
		reg_val = (macaddr[2] << 24) | (macaddr[3] << 16);
		reg_val |= (macaddr[4] << 8) | (macaddr[5]);

		ret = air_buckpbus_reg_write(phydev,
					     AN8801_BPBUS_REG_WOL_MAC_16_47,
					     reg_val);
		if (ret)
			return ret;

		/* MAC bits 0..15 */
		reg_val = (macaddr[0] << 8) | (macaddr[1]);

		ret = air_buckpbus_reg_write(phydev,
					     AN8801_BPBUS_REG_WOL_MAC_0_15,
					     reg_val);
		if (ret)
			return ret;

		ret = air_buckpbus_reg_set_bits(phydev,
						AN8801_BPBUS_REG_WAKEUP_CTL1,
						AN8801_WOL_WAKE_MAGIC_EN);
		if (ret)
			return ret;

		ret = air_buckpbus_reg_set_bits(phydev,
						AN8801_BPBUS_REG_WAKE_IRQ_EN,
						AN8801_IRQ_WAKE_MAGICPKT);
		if (ret)
			return ret;
	} else {
		ret = air_buckpbus_reg_clear_bits(phydev,
						  AN8801_BPBUS_REG_WAKEUP_CTL1,
						  AN8801_WOL_WAKE_MAGIC_EN);
		if (ret)
			return ret;

		ret = air_buckpbus_reg_clear_bits(phydev,
						  AN8801_BPBUS_REG_WAKE_IRQ_EN,
						  AN8801_IRQ_WAKE_MAGICPKT);
		if (ret)
			return ret;
	}

	return an8801r_ack_interrupt(phydev);
}

static int an8801r_of_init_leds(struct phy_device *phydev, u8 *led_cfg)
{
	struct device *dev = &phydev->mdio.dev;
	struct device_node *np = dev->of_node;
	struct device_node *leds;
	u32 function_enum_idx;
	int ret;

	if (!np)
		return 0;

	/* If devicetree is present, leds configuration is required */
	leds = of_get_child_by_name(np, "leds");
	if (!leds)
		return -ENOENT;

	for_each_available_child_of_node_scoped(leds, led) {
		u32 led_idx;

		ret = of_property_read_u32(led, "reg", &led_idx);
		if (ret)
			goto out;

		if (led_idx >= AN8801R_NUM_LEDS) {
			ret = -EINVAL;
			goto out;
		}

		ret = of_property_read_u32(led, "function-enumerator",
					   &function_enum_idx);
		if (ret)
			function_enum_idx = AN8801R_LED_FN_NONE;

		if (function_enum_idx >= AN8801R_LED_FN_MAX) {
			ret = -EINVAL;
			goto out;
		}

		led_cfg[led_idx] = function_enum_idx;
	}
out:
	of_node_put(leds);
	return ret;
}

static int an8801r_rgmii_rxdelay(struct phy_device *phydev, u16 delay_steps)
{
	u32 reg_val;

	if (delay_steps > RGMII_DELAY_STEP_MASK)
		return -EINVAL;

	reg_val = delay_steps & RGMII_DELAY_STEP_MASK;
	reg_val |= RGMII_RXDELAY_ALIGN;
	reg_val |= RGMII_RXDELAY_FORCE_MODE;

	return air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_RXDLY_STEP,
				      reg_val);
}

static int an8801r_rgmii_txdelay(struct phy_device *phydev, u16 delay_steps)
{
	u32 reg_val;

	if (delay_steps > RGMII_DELAY_STEP_MASK)
		return -EINVAL;

	reg_val = delay_steps & RGMII_DELAY_STEP_MASK;
	reg_val |= RGMII_TXDELAY_FORCE_MODE;

	return air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_TXDLY_STEP,
				      reg_val);
}

static int an8801r_rgmii_delay_config(struct phy_device *phydev)
{
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII_TXID:
		return an8801r_rgmii_txdelay(phydev, 4);
	case PHY_INTERFACE_MODE_RGMII_RXID:
		return an8801r_rgmii_rxdelay(phydev, 0);
	case PHY_INTERFACE_MODE_RGMII_ID:
		return an8801r_rgmii_txdelay(phydev, 4);
		return an8801r_rgmii_rxdelay(phydev, 0);
	case PHY_INTERFACE_MODE_RGMII:
	default:
		return 0;
	}
}

static int an8801r_config_init(struct phy_device *phydev)
{
	u8 led_default_function[AN8801R_NUM_LEDS] = { 0 };
	int prev_page, ret;

	ret = an8801r_of_init_leds(phydev, led_default_function);
	if (ret)
		return ret;

	/* Disable Low Power Mode (LPM) */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, AN8801_REG_PHY_INTERNAL0,
			    FIELD_PREP(AN8801_PHY_INTFUNC_MASK, 0x1e));
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, AN8801_REG_PHY_INTERNAL1,
			    FIELD_PREP(AN8801_PHY_INTFUNC_MASK, 0x2));
	if (ret)
		return ret;

	/* Disable EEE by default */
	ret = phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV, 0);
	if (ret)
		return ret;

	prev_page = phy_select_page(phydev, AIR_PHY_PAGE_EXTENDED_1);
	if (prev_page < 0)
		return prev_page;

	/* Set the PHY to perform auto-downshift after 3 auto-negotiation
	 * attempts
	 */
	__phy_write(phydev, AN8801_EXT_REG_PHY,
		    FIELD_PREP(AN8801_EXT_PHY_CTRL1, 0x1d) |
		    FIELD_PREP(AN8801_EXT_PHY_DOWNSHIFT_CTL, 1) |
		    AN8801_EXT_PHY_DOWNSHIFT_EN);

	ret = phy_restore_page(phydev, prev_page, ret);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_BYPASS_PTP,
				     AN8801_BYP_PTP_RGMII_TO_GPHY);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_EFIFO_CTL(0),
				     AN8801_EFIFO_RX_EN | AN8801_EFIFO_TX_EN |
				     AN8801_EFIFO_RX_CLK_EN |
				     AN8801_EFIFO_TX_CLK_EN |
				     AN8801_EFIFO_RX_EEE_EN |
				     AN8801_EFIFO_TX_EEE_EN);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_EFIFO_CTL(1),
				     AN8801_EFIFO_ALL_EN);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_EFIFO_CTL(2),
				     AN8801_EFIFO_ALL_EN);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1,
			    AN8801_PHY_TX_PAIR_DLY_SEL_GBE,
			    FIELD_PREP(AN8801_PHY_PAIR_DLY_SEL_A_GBE, 4) |
			    FIELD_PREP(AN8801_PHY_PAIR_DLY_SEL_C_GBE, 4));
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, AN8801_PHY_RXADC_CTRL,
			    AN8801_PHY_RXADC_SAMP_PHSEL_A |
			    AN8801_PHY_RXADC_SAMP_PHSEL_C);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, AN8801_PHY_RXADC_REV_0,
			    FIELD_PREP(AN8801_PHY_RXADC_REV_MASK_A, 1));
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, AN8801_PHY_RXADC_REV_1,
			    FIELD_PREP(AN8801_PHY_RXADC_REV_MASK_C, 1));
	if (ret)
		return ret;

	ret = an8801r_rgmii_delay_config(phydev);
	if (ret)
		return ret;

	ret = air_buckpbus_reg_write(phydev, AN8801_BPBUS_REG_CKO,
				     AN8801_CKO_OUTPUT_MODE_AUTO);
	if (ret)
		return ret;

	ret = an8801r_led_init(phydev, led_default_function);
	if (ret) {
		phydev_err(phydev, "Cannot initialize LEDs: %d\n", ret);
		return ret;
	}

	return 0;
}

static int an8801r_read_status(struct phy_device *phydev)
{
	int prev_speed, ret;
	u32 val;

	prev_speed = phydev->speed;

	ret = genphy_read_status(phydev);
	if (ret)
		return ret;

	if (!phydev->link)
		return 0;

	if (prev_speed != phydev->speed) {
		val = phydev->speed == SPEED_1000 ?
		      AN8801_BPBUS_LINK_MODE_1000 : 0;

		return air_buckpbus_reg_rmw(phydev, AN8801_BPBUS_REG_LINK_MODE,
					    AN8801_BPBUS_LINK_MODE_1000, val);
	};

	return 0;
}

static struct phy_driver airoha_driver[] = {
{
	PHY_ID_MATCH_MODEL(AN8801R_PHY_ID),
	.name			= "Airoha AN8801R",
	.features		= PHY_GBIT_FEATURES,
	.config_init		= an8801r_config_init,
	.suspend		= genphy_suspend,
	.resume			= genphy_resume,
	.config_aneg		= genphy_config_aneg,
	.read_status		= an8801r_read_status,
	.config_intr		= an8801r_config_intr,
	.handle_interrupt	= an8801r_handle_interrupt,
	.set_wol		= an8801r_set_wol,
	.get_wol		= an8801r_get_wol,
	.read_page		= an8801r_read_page,
	.write_page		= an8801r_write_page,
	.led_brightness_set	= an8801r_led_brightness_set,
	.led_blink_set		= an8801r_led_blink_set,
	.led_hw_is_supported	= an8801r_led_hw_is_supported,
	.led_hw_control_set	= an8801r_led_hw_control_set,
	.led_hw_control_get	= an8801r_led_hw_control_get,
	.led_polarity_set	= an8801r_led_polarity_set,
} };
module_phy_driver(airoha_driver);

static struct mdio_device_id __maybe_unused an8801_tbl[] = {
	{ PHY_ID_MATCH_MODEL(AN8801R_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, an8801_tbl);

MODULE_DESCRIPTION("Airoha AN8801 PHY driver");
MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_LICENSE("GPL");
