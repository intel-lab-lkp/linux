// SPDX-License-Identifier: GPL-2.0
/* Driver for the Texas Instruments DP83TG720 PHY
 * Copyright (c) 2023 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
 */
#include <linux/bitfield.h>
#include <linux/ethtool_netlink.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

#include "open_alliance_helpers.h"

#define DP83TG720_CS_1_1_PHY_ID			0x2000a284
#define DP83TG721_CS_1_0_PHY_ID			0x2000a290
#define MMD1F							0x1f
#define MMD1							0x1

/* MDIO_MMD_VEND2 registers */
#define DP83TG720_MII_REG_10			0x10
#define DP83TG720_STS_MII_INT			BIT(7)
#define DP83TG720_LINK_STATUS			BIT(0)

/* TDR Configuration Register (0x1E) */
#define DP83TG720_TDR_CFG				0x1e
/* 1b = TDR start, 0b = No TDR */
#define DP83TG720_TDR_START				BIT(15)
/* 1b = TDR auto on link down, 0b = Manual TDR start */
#define DP83TG720_CFG_TDR_AUTO_RUN		BIT(14)
/* 1b = TDR done, 0b = TDR in progress */
#define DP83TG720_TDR_DONE				BIT(1)
/* 1b = TDR fail, 0b = TDR success */
#define DP83TG720_TDR_FAIL				BIT(0)

#define DP83TG720_PHY_RESET_CTRL		0x1f
#define DP83TG720_HW_RESET			    BIT(15)
#define DP83TG720_SW_RESET              BIT(14)

#define DP83TG720_LPS_CFG3				0x18c
/* Power modes are documented as bit fields but used as values */
/* Power Mode 0 is Normal mode */
#define DP83TG720_LPS_CFG3_PWR_MODE_0	BIT(0)

/* Open Aliance 1000BaseT1 compatible HDD.TDR Fault Status Register */
#define DP83TG720_TDR_FAULT_STATUS		0x30f

/* Register 0x0301: TDR Configuration 2 */
#define DP83TG720_TDR_CFG2				0x301

/* Register 0x0303: TDR Configuration 3 */
#define DP83TG720_TDR_CFG3				0x303

/* Register 0x0304: TDR Configuration 4 */
#define DP83TG720_TDR_CFG4				0x304

/* Register 0x0405: Unknown Register */
#define DP83TG720_UNKNOWN_0405			0x405

/* Register 0x0576: TDR Master Link Down Control */
#define DP83TG720_TDR_MASTER_LINK_DOWN	0x576

#define DP83TG720_RGMII_DELAY_CTRL		0x602
/* In RGMII mode, Enable or disable the internal delay for RXD */
#define DP83TG720_RGMII_RX_CLK_SEL		BIT(1)
/* In RGMII mode, Enable or disable the internal delay for TXD */
#define DP83TG720_RGMII_TX_CLK_SEL		BIT(0)

/* Register 0x083F: Unknown Register */
#define DP83TG720_UNKNOWN_083F			0x83f

#define DP83TG720_SQI_REG_1				0x871
#define DP83TG720_SQI_OUT_WORST			GENMASK(7, 5)
#define DP83TG720_SQI_OUT				GENMASK(3, 1)

#define DP83TG720_SQI_MAX				7

/* SGMII CTRL Registers/bits */
#define DP83TG720_SGMII_CTRL			0x0608
#define SGMII_CONFIG_VAL				0x027B
#define DP83TG720_SGMII_AUTO_NEG_EN		BIT(0)
#define DP83TG720_SGMII_EN				BIT(9)

/* Strap Register/bits */
#define DP83TG720_STRAP					0x045d
#define DP83TG720_MASTER_MODE			BIT(5)
#define DP83TG720_RGMII_IS_EN			BIT(12)
#define DP83TG720_SGMII_IS_EN			BIT(13)
#define DP83TG720_RX_SHIFT_EN			BIT(14)
#define DP83TG720_TX_SHIFT_EN			BIT(15)

enum DP83TG720_chip_type {
	DP83TG720_CS1_1,
	DP83TG721_CS1,
};

struct DP83TG720_private {
	int chip;
	bool is_master;
	bool is_rgmii;
	bool is_sgmii;
	bool rx_shift;
	bool tx_shift;
};

struct DP83TG720_init_reg {
	int MMD;
	int reg;
	int val;
};

/*Refer to SNLA371 for more information*/
static const struct DP83TG720_init_reg DP83TG720_cs1_1_master_init[] = {
	{0x1F, 0x001F, 0X8000},
	{0x1F, 0x0573, 0x0101},
	{0x1, 0x0834, 0xC001},
	{0x1F, 0x0405, 0x5800},
	{0x1F, 0x08AD, 0x3C51},
	{0x1F, 0x0894, 0x5DF7},
	{0x1F, 0x08A0, 0x09E7},
	{0x1F, 0x08C0, 0x4000},
	{0x1F, 0x0814, 0x4800},
	{0x1F, 0x080D, 0x2EBF},
	{0x1F, 0x08C1, 0x0B00},
	{0x1F, 0x087D, 0x0001},
	{0x1F, 0x082E, 0x0000},
	{0x1F, 0x0837, 0x00F4},
	{0x1F, 0x08BE, 0x0200},
	{0x1F, 0x08C5, 0x4000},
	{0x1F, 0x08C7, 0x2000},
	{0x1F, 0x08B3, 0x005A},
	{0x1F, 0x08B4, 0x005A},
	{0x1F, 0x08B0, 0x0202},
	{0x1F, 0x08B5, 0x00EA},
	{0x1F, 0x08BA, 0x2828},
	{0x1F, 0x08BB, 0x6828},
	{0x1F, 0x08BC, 0x0028},
	{0x1F, 0x08BF, 0x0000},
	{0x1F, 0x08B1, 0x0014},
	{0x1F, 0x08B2, 0x0008},
	{0x1F, 0x08EC, 0x0000},
	{0x1F, 0x08C8, 0x0003},
	{0x1F, 0x08BE, 0x0201},
	{0x1F, 0x018C, 0x0001},
	{0x1F, 0x001F, 0x4000},
	{0x1F, 0x0573, 0x0001},
	{0x1F, 0x056A, 0x5F41},
};

/*Refer to SNLA371 for more information*/
static const struct DP83TG720_init_reg DP83TG720_cs1_1_slave_init[] = {
	{0x1F, 0x001F, 0x8000},
	{0x1F, 0x0573, 0x0101},
	{0x1, 0x0834, 0x8001},
	{0x1F, 0x0894, 0x5DF7},
	{0x1F, 0x056a, 0x5F40},
	{0x1F, 0x0405, 0x5800},
	{0x1F, 0x08AD, 0x3C51},
	{0x1F, 0x0894, 0x5DF7},
	{0x1F, 0x08A0, 0x09E7},
	{0x1F, 0x08C0, 0x4000},
	{0x1F, 0x0814, 0x4800},
	{0x1F, 0x080D, 0x2EBF},
	{0x1F, 0x08C1, 0x0B00},
	{0x1F, 0x087d, 0x0001},
	{0x1F, 0x082E, 0x0000},
	{0x1F, 0x0837, 0x00f4},
	{0x1F, 0x08BE, 0x0200},
	{0x1F, 0x08C5, 0x4000},
	{0x1F, 0x08C7, 0x2000},
	{0x1F, 0x08B3, 0x005A},
	{0x1F, 0x08B4, 0x005A},
	{0x1F, 0x08B0, 0x0202},
	{0x1F, 0x08B5, 0x00EA},
	{0x1F, 0x08BA, 0x2828},
	{0x1F, 0x08BB, 0x6828},
	{0x1F, 0x08BC, 0x0028},
	{0x1F, 0x08BF, 0x0000},
	{0x1F, 0x08B1, 0x0014},
	{0x1F, 0x08B2, 0x0008},
	{0x1F, 0x08EC, 0x0000},
	{0x1F, 0x08C8, 0x0003},
	{0x1F, 0x08BE, 0x0201},
	{0x1F, 0x056A, 0x5F40},
	{0x1F, 0x018C, 0x0001},
	{0x1F, 0x001F, 0x4000},
	{0x1F, 0x0573, 0x0001},
	{0x1F, 0x056A, 0X5F41},
};

/*Refer to SNLA371 for more information*/
static const struct DP83TG720_init_reg DP83TG721_cs1_master_init[] = {
	{0x1F, 0x001F, 0x8000},
	{0x1F, 0x0573, 0x0801},
	{0x1, 0x0834, 0xC001},
	{0x1F, 0x0405, 0x6C00},
	{0x1F, 0x08AD, 0x3C51},
	{0x1F, 0x0894, 0x5DF7},
	{0x1F, 0x08A0, 0x09E7},
	{0x1F, 0x08C0, 0x4000},
	{0x1F, 0x0814, 0x4800},
	{0x1F, 0x080D, 0x2EBF},
	{0x1F, 0x08C1, 0x0B00},
	{0x1F, 0x087D, 0x0001},
	{0x1F, 0x082E, 0x0000},
	{0x1F, 0x0837, 0x00F8},
	{0x1F, 0x08BE, 0x0200},
	{0x1F, 0x08C5, 0x4000},
	{0x1F, 0x08C7, 0x2000},
	{0x1F, 0x08B3, 0x005A},
	{0x1F, 0x08B4, 0x005A},
	{0x1F, 0x08B0, 0x0202},
	{0x1F, 0x08B5, 0x00EA},
	{0x1F, 0x08BA, 0x2828},
	{0x1F, 0x08BB, 0x6828},
	{0x1F, 0x08BC, 0x0028},
	{0x1F, 0x08BF, 0x0000},
	{0x1F, 0x08B1, 0x0014},
	{0x1F, 0x08B2, 0x0008},
	{0x1F, 0x08EC, 0x0000},
	{0x1F, 0x08FC, 0x0091},
	{0x1F, 0x08BE, 0x0201},
	{0x1F, 0x0335, 0x0010},
	{0x1F, 0x0336, 0x0009},
	{0x1F, 0x0337, 0x0208},
	{0x1F, 0x0338, 0x0208},
	{0x1F, 0x0339, 0x02CB},
	{0x1F, 0x033A, 0x0208},
	{0x1F, 0x033B, 0x0109},
	{0x1F, 0x0418, 0x0380},
	{0x1F, 0x0420, 0xFF10},
	{0x1F, 0x0421, 0x4033},
	{0x1F, 0x0422, 0x0800},
	{0x1F, 0x0423, 0x0002},
	{0x1F, 0x0484, 0x0003},
	{0x1F, 0x055D, 0x0008},
	{0x1F, 0x042B, 0x0018},
	{0x1F, 0x087C, 0x0080},
	{0x1F, 0x08C1, 0x0900},
	{0x1F, 0x08fc, 0x4091},
	{0x1F, 0x0881, 0x5146},
	{0x1F, 0x08be, 0x02a1},
	{0x1F, 0x0867, 0x9999},
	{0x1F, 0x0869, 0x9666},
	{0x1F, 0x086a, 0x0009},
	{0x1F, 0x0822, 0x11e1},
	{0x1F, 0x08f9, 0x1f11},
	{0x1F, 0x08a3, 0x24e8},
	{0x1F, 0x018C, 0x0001},
	{0x1F, 0x001F, 0x4000},
	{0x1F, 0x0573, 0x0001},
	{0x1F, 0x056A, 0x5F41},
};

/*Refer to SNLA371 for more information*/
static const struct DP83TG720_init_reg DP83TG721_cs1_slave_init[] = {
	{0x1F, 0x001F, 0x8000},
	{0x1F, 0x0573, 0x0801},
	{0x1, 0x0834, 0x8001},
	{0x1F, 0x0405, 0X6C00},
	{0x1F, 0x08AD, 0x3C51},
	{0x1F, 0x0894, 0x5DF7},
	{0x1F, 0x08A0, 0x09E7},
	{0x1F, 0x08C0, 0x4000},
	{0x1F, 0x0814, 0x4800},
	{0x1F, 0x080D, 0x2EBF},
	{0x1F, 0x08C1, 0x0B00},
	{0x1F, 0x087D, 0x0001},
	{0x1F, 0x082E, 0x0000},
	{0x1F, 0x0837, 0x00F8},
	{0x1F, 0x08BE, 0x0200},
	{0x1F, 0x08C5, 0x4000},
	{0x1F, 0x08C7, 0x2000},
	{0x1F, 0x08B3, 0x005A},
	{0x1F, 0x08B4, 0x005A},
	{0x1F, 0x08B0, 0x0202},
	{0x1F, 0x08B5, 0x00EA},
	{0x1F, 0x08BA, 0x2828},
	{0x1F, 0x08BB, 0x6828},
	{0x1F, 0x08BC, 0x0028},
	{0x1F, 0x08BF, 0x0000},
	{0x1F, 0x08B1, 0x0014},
	{0x1F, 0x08B2, 0x0008},
	{0x1F, 0x08EC, 0x0000},
	{0x1F, 0x08FC, 0x0091},
	{0x1F, 0x08BE, 0x0201},
	{0x1F, 0x0456, 0x0160},
	{0x1F, 0x0335, 0x0010},
	{0x1F, 0x0336, 0x0009},
	{0x1F, 0x0337, 0x0208},
	{0x1F, 0x0338, 0x0208},
	{0x1F, 0x0339, 0x02CB},
	{0x1F, 0x033A, 0x0208},
	{0x1F, 0x033B, 0x0109},
	{0x1F, 0x0418, 0x0380},
	{0x1F, 0x0420, 0xFF10},
	{0x1F, 0x0421, 0x4033},
	{0x1F, 0x0422, 0x0800},
	{0x1F, 0x0423, 0x0002},
	{0x1F, 0x0484, 0x0003},
	{0x1F, 0x055D, 0x0008},
	{0x1F, 0x042B, 0x0018},
	{0x1F, 0x082D, 0x120F},
	{0x1F, 0x0888, 0x0438},
	{0x1F, 0x0824, 0x09E0},
	{0x1F, 0x0883, 0x5146},
	{0x1F, 0x08BE, 0x02A1},
	{0x1F, 0x0822, 0x11E1},
	{0x1F, 0x056A, 0x5F40},
	{0x1F, 0x08C1, 0x0900},
	{0x1F, 0x08FC, 0x4091},
	{0x1F, 0x08F9, 0x1F11},
	{0x1F, 0x084F, 0x290C},
	{0x1F, 0x0850, 0x3D33},
	{0x1F, 0x018C, 0x0001},
	{0x1F, 0x001F, 0x4000},
	{0x1F, 0x0573, 0x0001},
	{0x1F, 0x056A, 0x5F41},
};

static int dp83tg720_read_straps(struct phy_device *phydev)
{
	struct DP83TG720_private *DP83TG720 = phydev->priv;
	int strap;

	strap = phy_read_mmd(phydev, MMD1F, DP83TG720_STRAP);
	if (strap < 0)
		return strap;

	if (strap & DP83TG720_MASTER_MODE)
		DP83TG720->is_master = true;

	if (strap & DP83TG720_RGMII_IS_EN)
		DP83TG720->is_rgmii = true;

	if (strap & DP83TG720_SGMII_IS_EN)
		DP83TG720->is_sgmii = true;

	if (strap & DP83TG720_RX_SHIFT_EN)
		DP83TG720->rx_shift = true;

	if (strap & DP83TG720_TX_SHIFT_EN)
		DP83TG720->tx_shift = true;

	return 0;
};

static int dp83tg720_reset(struct phy_device *phydev, bool hw_reset)
{
	int ret;

	if (hw_reset)
		ret = phy_write_mmd(phydev, MMD1F, DP83TG720_PHY_RESET_CTRL,
				    DP83TG720_HW_RESET);
	else
		ret = phy_write_mmd(phydev, MMD1F, DP83TG720_PHY_RESET_CTRL,
				    DP83TG720_SW_RESET);
	if (ret)
		return ret;

	mdelay(100);

	return 0;
}

static int dp83tg720_phy_reset(struct phy_device *phydev)
{
	int ret;

	ret = dp83tg720_reset(phydev, false);
	if (ret)
		return ret;

	ret = dp83tg720_read_straps(phydev);
	if (ret)
		return ret;

	return 0;
}

static int DP83TG720_write_seq(struct phy_device *phydev,
			       const struct DP83TG720_init_reg *init_data,
			       int size)
{
	int ret;
	int i;

	for (i = 0; i < size; i++) {
		ret = phy_write_mmd(phydev, init_data[i].MMD, init_data[i].reg,
				    init_data[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * dp83tg720_cable_test_start - Start the cable test for the DP83TG720 PHY.
 * @phydev: Pointer to the phy_device structure.
 *
 * This sequence is based on the documented procedure for the DP83TG720 PHY.
 *
 * Returns: 0 on success, a negative error code on failure.
 */
static int dp83tg720_cable_test_start(struct phy_device *phydev)
{
	int ret;

	/* Initialize the PHY to run the TDR test as described in the
	 * "DP83TG720-Q1: Configuring for Open Alliance Specification
	 * Compliance (Rev. B)" application note.
	 * Most of the registers are not documented. Some of register names
	 * are guessed by comparing the register offsets with the DP83TD510E.
	 */

	/* Force master link down */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2,
			       DP83TG720_TDR_MASTER_LINK_DOWN, 0x0400);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_TDR_CFG2,
			    0xa008);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_TDR_CFG3,
			    0x0928);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_TDR_CFG4,
			    0x0004);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_UNKNOWN_0405,
			    0x6400);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_UNKNOWN_083F,
			    0x3003);
	if (ret)
		return ret;

	/* Start the TDR */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_TDR_CFG,
			       DP83TG720_TDR_START);
	if (ret)
		return ret;

	return 0;
}

/**
 * dp83tg720_cable_test_get_status - Get the status of the cable test for the
 *                                   DP83TG720 PHY.
 * @phydev: Pointer to the phy_device structure.
 * @finished: Pointer to a boolean that indicates whether the test is finished.
 *
 * The function sets the @finished flag to true if the test is complete.
 *
 * Returns: 0 on success or a negative error code on failure.
 */
static int dp83tg720_cable_test_get_status(struct phy_device *phydev,
					   bool *finished)
{
	int ret, stat;

	*finished = false;

	/* Read the TDR status */
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_TDR_CFG);
	if (ret < 0)
		return ret;

	/* Check if the TDR test is done */
	if (!(ret & DP83TG720_TDR_DONE))
		return 0;

	/* Check for TDR test failure */
	if (!(ret & DP83TG720_TDR_FAIL)) {
		int location;

		/* Read fault status */
		ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
				   DP83TG720_TDR_FAULT_STATUS);
		if (ret < 0)
			return ret;

		/* Get fault type */
		stat = oa_1000bt1_get_ethtool_cable_result_code(ret);

		/* Determine fault location */
		location = oa_1000bt1_get_tdr_distance(ret);
		if (location > 0)
			ethnl_cable_test_fault_length(phydev,
						      ETHTOOL_A_CABLE_PAIR_A,
						      location);
	} else {
		/* Active link partner or other issues */
		stat = ETHTOOL_A_CABLE_RESULT_CODE_UNSPEC;
	}

	*finished = true;

	ethnl_cable_test_result(phydev, ETHTOOL_A_CABLE_PAIR_A, stat);

	return phy_init_hw(phydev);
}

static int dp83tg720_config_aneg(struct phy_device *phydev)
{
	int ret;

	/* Autoneg is not supported and this PHY supports only one speed.
	 * We need to care only about master/slave configuration if it was
	 * changed by user.
	 */
	ret = genphy_c45_pma_baset1_setup_master_slave(phydev);
	if (ret)
		return ret;

	/* Re-read role configuration to make changes visible even if
	 * the link is in administrative down state.
	 */
	return genphy_c45_pma_baset1_read_master_slave(phydev);
}

static int dp83tg720_read_status(struct phy_device *phydev)
{
	u16 phy_sts;
	int ret;

	phydev->pause = 0;
	phydev->asym_pause = 0;

	/* Most of Clause 45 registers are not present, so we can't use
	 * genphy_c45_read_status() here.
	 */
	phy_sts = phy_read(phydev, DP83TG720_MII_REG_10);
	phydev->link = !!(phy_sts & DP83TG720_LINK_STATUS);
	if (!phydev->link) {
		/* According to the "DP83TC81x, DP83TG72x Software
		 * Implementation Guide", the PHY needs to be reset after a
		 * link loss or if no link is created after at least 100ms.
		 *
		 * Currently we are polling with the PHY_STATE_TIME (1000ms)
		 * interval, which is still enough for not automotive use cases.
		 */
		ret = phy_init_hw(phydev);
		if (ret)
			return ret;

		/* After HW reset we need to restore master/slave configuration.
		 * genphy_c45_pma_baset1_read_master_slave() call will be done
		 * by the dp83tg720_config_aneg() function.
		 */
		ret = dp83tg720_config_aneg(phydev);
		if (ret)
			return ret;

		phydev->speed = SPEED_UNKNOWN;
		phydev->duplex = DUPLEX_UNKNOWN;
	} else {
		/* PMA/PMD control 1 register (Register 1.0) is present, but it
		 * doesn't contain the link speed information.
		 * So genphy_c45_read_pma() can't be used here.
		 */
		ret = genphy_c45_pma_baset1_read_master_slave(phydev);
		if (ret)
			return ret;

		phydev->duplex = DUPLEX_FULL;
		phydev->speed = SPEED_1000;
	}

	return 0;
}

static int dp83tg720_get_sqi(struct phy_device *phydev)
{
	int ret;

	if (!phydev->link)
		return 0;

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_SQI_REG_1);
	if (ret < 0)
		return ret;

	return FIELD_GET(DP83TG720_SQI_OUT, ret);
}

static int dp83tg720_get_sqi_max(struct phy_device *phydev)
{
	return DP83TG720_SQI_MAX;
}

static int dp83tg720_config_rgmii_delay(struct phy_device *phydev)
{
	u16 rgmii_delay_mask;
	u16 rgmii_delay = 0;

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		rgmii_delay = 0;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		rgmii_delay = DP83TG720_RGMII_RX_CLK_SEL |
				DP83TG720_RGMII_TX_CLK_SEL;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		rgmii_delay = DP83TG720_RGMII_RX_CLK_SEL;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		rgmii_delay = DP83TG720_RGMII_TX_CLK_SEL;
		break;
	default:
		return 0;
	}

	rgmii_delay_mask = DP83TG720_RGMII_RX_CLK_SEL |
		DP83TG720_RGMII_TX_CLK_SEL;

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2,
			      DP83TG720_RGMII_DELAY_CTRL, rgmii_delay_mask,
			      rgmii_delay);
}

static int dp83tg720_chip_init(struct phy_device *phydev)
{
	struct DP83TG720_private *DP83TG720 = phydev->priv;
	int ret;

	ret = dp83tg720_reset(phydev, true);
	if (ret)
		return ret;

	phydev->autoneg = AUTONEG_DISABLE;
	phydev->speed = SPEED_1000;
	phydev->duplex = DUPLEX_FULL;
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported);

	switch (DP83TG720->chip) {
	case DP83TG720_CS1_1:
		if (DP83TG720->is_master)
			ret = DP83TG720_write_seq(phydev, DP83TG720_cs1_1_master_init,
						  ARRAY_SIZE(DP83TG720_cs1_1_master_init));
		else
			ret = DP83TG720_write_seq(phydev, DP83TG720_cs1_1_slave_init,
						  ARRAY_SIZE(DP83TG720_cs1_1_slave_init));

		ret = dp83tg720_reset(phydev, false);

		return 1;
	case DP83TG721_CS1:
		if (DP83TG720->is_master)
			ret = DP83TG720_write_seq(phydev, DP83TG721_cs1_master_init,
						  ARRAY_SIZE(DP83TG721_cs1_master_init));
		else
			ret = DP83TG720_write_seq(phydev, DP83TG721_cs1_slave_init,
						  ARRAY_SIZE(DP83TG721_cs1_slave_init));

		ret = dp83tg720_reset(phydev, false);

		return 1;
	default:
		return -EINVAL;
	};

	if (ret)
		return ret;

	/* Enable the PHY */
	ret = phy_write_mmd(phydev, MMD1F, DP83TG720_LPS_CFG3, DP83TG720_LPS_CFG3_PWR_MODE_0);
	if (ret)
		return ret;

	mdelay(10);

	/* Do a software reset to restart the PHY with the updated values */
	return dp83tg720_reset(phydev, false);
}

static int dp83tg720_config_init(struct phy_device *phydev)
{
	int value, ret;

	/* Software Restart is not enough to recover from a link failure.
	 * Using Hardware Reset instead.
	 */
	ret = dp83tg720_chip_init(phydev);

	/* Wait until MDC can be used again.
	 * The wait value of one 1ms is documented in "DP83TG720-Q1 1000BASE-T1
	 * Automotive Ethernet PHY with SGMII and RGMII" datasheet.
	 */
	usleep_range(1000, 2000);

	if (phy_interface_is_rgmii(phydev)) {
		ret = dp83tg720_config_rgmii_delay(phydev);
		if (ret)
			return ret;
	}

	value = phy_read_mmd(phydev, MMD1F, DP83TG720_SGMII_CTRL);
	if (value < 0)
		return value;

	if (phydev->interface == PHY_INTERFACE_MODE_SGMII)
		value |= DP83TG720_SGMII_EN;
	else
		value &= ~DP83TG720_SGMII_EN;

	ret = phy_write_mmd(phydev, MMD1F, DP83TG720_SGMII_CTRL, value);
	if (ret < 0)
		return ret;

	/* In case the PHY is bootstrapped in managed mode, we need to
	 * wake it.
	 */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720_LPS_CFG3,
			    DP83TG720_LPS_CFG3_PWR_MODE_0);
	if (ret)
		return ret;

	/* Make role configuration visible for ethtool on init and after
	 * rest.
	 */
	return genphy_c45_pma_baset1_read_master_slave(phydev);
}

static int dp83tg720_probe(struct phy_device *phydev)
{
	struct DP83TG720_private *DP83TG720;
	int ret;

	DP83TG720 = devm_kzalloc(&phydev->mdio.dev, sizeof(*DP83TG720),
				 GFP_KERNEL);
	if (!DP83TG720)
		return -ENOMEM;

	phydev->priv = DP83TG720;

	ret = dp83tg720_read_straps(phydev);
	if (ret)
		return ret;

	switch (phydev->phy_id) {
	case DP83TG720_CS_1_1_PHY_ID:
		DP83TG720->chip = DP83TG720_CS1_1;
		break;
	case DP83TG721_CS_1_0_PHY_ID:
		DP83TG720->chip = DP83TG721_CS1;
		break;
	default:
		return -EINVAL;
	};

	return dp83tg720_config_init(phydev);
}

#define DP83TG720_PHY_DRIVER(_id, _name)				\
{									\
	PHY_ID_MATCH_EXACT(_id),					\
	.name                   = (_name),				\
	.probe                  = dp83tg720_probe,			\
	.soft_reset		= dp83tg720_phy_reset,			\
	.flags                  = PHY_POLL_CABLE_TEST,			\
	.config_aneg            = dp83tg720_config_aneg,		\
	.read_status            = dp83tg720_read_status,		\
	.get_features           = genphy_c45_pma_read_ext_abilities,	\
	.config_init            = dp83tg720_config_init,		\
	.get_sqi                = dp83tg720_get_sqi,			\
	.get_sqi_max            = dp83tg720_get_sqi_max,		\
	.cable_test_start       = dp83tg720_cable_test_start,		\
	.cable_test_get_status  = dp83tg720_cable_test_get_status,	\
	.suspend                = genphy_suspend,			\
	.resume                 = genphy_resume,			\
}

static struct phy_driver dp83tg720_driver[] = {
	DP83TG720_PHY_DRIVER(DP83TG720_CS_1_1_PHY_ID, "TI DP83TG720CS1.1"),
	DP83TG720_PHY_DRIVER(DP83TG721_CS_1_0_PHY_ID, "TI DP83TG721CS1.0"),
};
module_phy_driver(dp83tg720_driver);

static struct mdio_device_id __maybe_unused dp83tg720_tbl[] = {
	{ PHY_ID_MATCH_EXACT(DP83TG720_CS_1_1_PHY_ID) },
	{ PHY_ID_MATCH_EXACT(DP83TG721_CS_1_0_PHY_ID) },
	{ },
};
MODULE_DEVICE_TABLE(mdio, dp83tg720_tbl);

MODULE_DESCRIPTION("Texas Instruments DP83TG720 PHY driver");
MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
MODULE_LICENSE("GPL");
