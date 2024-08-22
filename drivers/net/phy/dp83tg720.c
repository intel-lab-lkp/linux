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

#define DP83TG720S_PHY_ID			0x2000a284

/* MDIO_MMD_VEND2 registers */
#define DP83TG720S_MII_REG_10			0x10
#define DP83TG720S_STS_MII_INT			BIT(7)
#define DP83TG720S_LINK_STATUS			BIT(0)

/* TDR Configuration Register (0x1E) */
#define DP83TG720S_TDR_CFG			0x1e
/* 1b = TDR start, 0b = No TDR */
#define DP83TG720S_TDR_START			BIT(15)
/* 1b = TDR auto on link down, 0b = Manual TDR start */
#define DP83TG720S_CFG_TDR_AUTO_RUN		BIT(14)
/* 1b = TDR done, 0b = TDR in progress */
#define DP83TG720S_TDR_DONE			BIT(1)
/* 1b = TDR fail, 0b = TDR success */
#define DP83TG720S_TDR_FAIL			BIT(0)

#define DP83TG720S_PHY_RESET			0x1f
#define DP83TG720S_HW_RESET			BIT(15)

#define DP83TG720S_LPS_CFG3			0x18c
/* Power modes are documented as bit fields but used as values */
/* Power Mode 0 is Normal mode */
#define DP83TG720S_LPS_CFG3_PWR_MODE_0		BIT(0)

/* Open Aliance 1000BaseT1 compatible HDD.TDR Fault Status Register */
#define DP83TG720S_TDR_FAULT_STATUS		0x30f

/* Register 0x0301: TDR Configuration 2 */
#define DP83TG720S_TDR_CFG2			0x301

/* Register 0x0303: TDR Configuration 3 */
#define DP83TG720S_TDR_CFG3			0x303

/* Register 0x0304: TDR Configuration 4 */
#define DP83TG720S_TDR_CFG4			0x304

/* Register 0x0405: Unknown Register */
#define DP83TG720S_UNKNOWN_0405			0x405

#define DP83TG720S_A2D_REG_66			0x442
#define DP83TG720S_ESD_EVENT_COUNT_MASK		GENMASK(14, 9)

#define DP83TG720S_LINK_QUAL_1			0x543
#define DP83TG720S_LINK_TRAINING_TIME_MASK	GENMASK(7, 0)

#define DP83TG720S_LINK_QUAL_2			0x544
#define DP83TG720S_REMOTE_RECEIVER_TIME_MASK	GENMASK(15, 8)
#define DP83TG720S_LOCAL_RECEIVER_TIME_MASK	GENMASK(7, 0)

#define DP83TG720S_LINK_QUAL_3			0x547
#define DP83TG720S_LINK_LOSS_CNT_MASK		GENMASK(15, 10)
#define DP83TG720S_LINK_FAIL_CNT_MASK		GENMASK(9, 0)

/* Register 0x0576: TDR Master Link Down Control */
#define DP83TG720S_TDR_MASTER_LINK_DOWN		0x576

#define DP83TG720S_RGMII_DELAY_CTRL		0x602
/* In RGMII mode, Enable or disable the internal delay for RXD */
#define DP83TG720S_RGMII_RX_CLK_SEL		BIT(1)
/* In RGMII mode, Enable or disable the internal delay for TXD */
#define DP83TG720S_RGMII_TX_CLK_SEL		BIT(0)

#define DP83TG720S_PKT_STAT_1			0x639
#define DP83TG720S_TX_PKT_CNT_15_0_MASK		GENMASK(15, 0)

#define DP83TG720S_PKT_STAT_2			0x63A
#define DP83TG720S_TX_PKT_CNT_31_16_MASK	GENMASK(15, 0)

#define DP83TG720S_PKT_STAT_3			0x63B
#define DP83TG720S_TX_ERR_PKT_CNT_MASK		GENMASK(15, 0)

#define DP83TG720S_PKT_STAT_4			0x63C
#define DP83TG720S_RX_PKT_CNT_15_0_MASK		GENMASK(15, 0)

#define DP83TG720S_PKT_STAT_5			0x63D
#define DP83TG720S_RX_PKT_CNT_31_16_MASK	GENMASK(15, 0)

#define DP83TG720S_PKT_STAT_6			0x63E
#define DP83TG720S_RX_ERR_PKT_CNT_MASK		GENMASK(15, 0)

/* Register 0x083F: Unknown Register */
#define DP83TG720S_UNKNOWN_083F			0x83f

#define DP83TG720S_SQI_REG_1			0x871
#define DP83TG720S_SQI_OUT_WORST		GENMASK(7, 5)
#define DP83TG720S_SQI_OUT			GENMASK(3, 1)

#define DP83TG720_SQI_MAX			7

struct dp83tg720_cache_reg {
	u16 mmd;
	u16 reg;
};

#define DP83TG720_FLAG_COUNTER BIT(0)

struct dp83tg720_hw_stat {
	const char *string;
	u8 cache_index1;
	u8 cache_index2;  /* If a statistic spans multiple registers */
	u32 mask1;
	u32 mask2;        /* Mask for the second register */
	u8 shift1;
	u8 shift2;        /* Shift for the second register */
	u8 flags;
};

enum dp83tg720_cache_reg_idx {
	DP83TG720_CACHE_A2D_REG_66 = 0,
	DP83TG720_CACHE_LINK_QUAL_1,
	DP83TG720_CACHE_LINK_QUAL_2,
	DP83TG720_CACHE_LINK_QUAL_3,
	DP83TG720_CACHE_PKT_STAT_1,
	DP83TG720_CACHE_PKT_STAT_2,
	DP83TG720_CACHE_PKT_STAT_3,
	DP83TG720_CACHE_PKT_STAT_4,
	DP83TG720_CACHE_PKT_STAT_5,
	DP83TG720_CACHE_PKT_STAT_6,

	DP83TG720_CACHE_EMPTY,
};

static const struct dp83tg720_cache_reg dp83tg720_cache_regs[] = {
	[DP83TG720_CACHE_A2D_REG_66] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_A2D_REG_66,
	},
	[DP83TG720_CACHE_LINK_QUAL_1] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_LINK_QUAL_1,
	},
	[DP83TG720_CACHE_LINK_QUAL_2] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_LINK_QUAL_2,
	},
	[DP83TG720_CACHE_LINK_QUAL_3] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_LINK_QUAL_3,
	},
	[DP83TG720_CACHE_PKT_STAT_1] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_1,
	},
	[DP83TG720_CACHE_PKT_STAT_2] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_2,
	},
	[DP83TG720_CACHE_PKT_STAT_3] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_3,
	},
	[DP83TG720_CACHE_PKT_STAT_4] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_4,
	},
	[DP83TG720_CACHE_PKT_STAT_5] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_5,
	},
	[DP83TG720_CACHE_PKT_STAT_6] = {
		.mmd = MDIO_MMD_VEND2,
		.reg = DP83TG720S_PKT_STAT_6,
	},
};

static const struct dp83tg720_hw_stat dp83tg720_hw_stats[] = {
	{
		.string = OA_LQ_LFL_ESD_EVENT_COUNT,
		.cache_index1 = DP83TG720_CACHE_A2D_REG_66,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_ESD_EVENT_COUNT_MASK,
		.shift1 = 9,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = OA_LQ_LINK_TRAINING_TIME,
		.cache_index1 = DP83TG720_CACHE_LINK_QUAL_1,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_LINK_TRAINING_TIME_MASK,
	},
	{
		.string = OA_LQ_REMOTE_RECEIVER_TIME,
		.cache_index1 = DP83TG720_CACHE_LINK_QUAL_2,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_REMOTE_RECEIVER_TIME_MASK,
		.shift1 = 8,
	},
	{
		.string = OA_LQ_LOCAL_RECEIVER_TIME,
		.cache_index1 = DP83TG720_CACHE_LINK_QUAL_2,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_LOCAL_RECEIVER_TIME_MASK,
	},
	{
		.string = OA_LQ_LFL_LINK_LOSS_COUNT,
		.cache_index1 = DP83TG720_CACHE_LINK_QUAL_3,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_LINK_LOSS_CNT_MASK,
		.shift1 = 10,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = OA_LQ_LFL_LINK_FAILURE_COUNT,
		.cache_index1 = DP83TG720_CACHE_LINK_QUAL_3,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_LINK_FAIL_CNT_MASK,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = PHY_TX_PKT_COUNT,
		.cache_index1 = DP83TG720_CACHE_PKT_STAT_1,
		.cache_index2 = DP83TG720_CACHE_PKT_STAT_2,
		.mask1 = DP83TG720S_TX_PKT_CNT_15_0_MASK,
		.mask2 = DP83TG720S_TX_PKT_CNT_31_16_MASK,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = PHY_TX_ERR_COUNT,
		.cache_index1 = DP83TG720_CACHE_PKT_STAT_3,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_TX_ERR_PKT_CNT_MASK,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = PHY_RX_PKT_COUNT,
		.cache_index1 = DP83TG720_CACHE_PKT_STAT_4,
		.cache_index2 = DP83TG720_CACHE_PKT_STAT_5,
		.mask1 = DP83TG720S_RX_PKT_CNT_15_0_MASK,
		.mask2 = DP83TG720S_RX_PKT_CNT_31_16_MASK,
		.flags = DP83TG720_FLAG_COUNTER,
	},
	{
		.string = PHY_RX_ERR_COUNT,
		.cache_index1 = DP83TG720_CACHE_PKT_STAT_6,
		.cache_index2 = DP83TG720_CACHE_EMPTY,
		.mask1 = DP83TG720S_RX_ERR_PKT_CNT_MASK,
		.flags = DP83TG720_FLAG_COUNTER,
	},
};

struct dp83tg720_priv {
	u64 stats[ARRAY_SIZE(dp83tg720_hw_stats)];
	u16 reg_cache[ARRAY_SIZE(dp83tg720_cache_regs)];
};

/**
 * dp83tg720_cache_reg_values - Cache register values to avoid clearing counters.
 * @phydev: Pointer to the phy_device structure.
 *
 * Reads and caches the values of all relevant registers.
 *
 * Returns: 0 on success, a negative error code on failure.
 */
static int dp83tg720_cache_reg_values(struct phy_device *phydev)
{
	struct dp83tg720_priv *priv = phydev->priv;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(dp83tg720_cache_regs); i++) {
		const struct dp83tg720_cache_reg *cache_reg =
			&dp83tg720_cache_regs[i];

		ret = phy_read_mmd(phydev, cache_reg->mmd, cache_reg->reg);
		if (ret < 0)
			return ret;

		priv->reg_cache[i] = (u16)ret;
	}

	return 0;
}

/**
 * dp83tg720_extract_stat_value - Extract specific statistic value from cache.
 * @phydev: Pointer to the phy_device structure.
 * @i: Index of the statistic in the dp83tg720_hw_stats array.
 *
 * Extracts the specific statistic value from the cached register values.
 *
 * Returns: The extracted statistic value.
 */
static u64 dp83tg720_extract_stat_value(struct phy_device *phydev, int i)
{
	const struct dp83tg720_hw_stat *stat = &dp83tg720_hw_stats[i];
	struct dp83tg720_priv *priv = phydev->priv;
	u32 val1, val2 = 0;

	val1 = (priv->reg_cache[stat->cache_index1] & stat->mask1);
	val1 >>= stat->shift1;
	if (stat->cache_index2 != DP83TG720_CACHE_EMPTY) {
		val2 = (priv->reg_cache[stat->cache_index2] & stat->mask2);
		val2 >>= stat->shift2;
	}

	if (stat->flags & DP83TG720_FLAG_COUNTER)
		priv->stats[i] += val1 | (val2 << 16);
	else
		priv->stats[i] = val1 | (val2 << 16);

	return priv->stats[i];
}

/**
 * dp83tg720_get_sset_count - Get the number of statistics sets.
 * @phydev: Pointer to the phy_device structure.
 *
 * Returns: The number of statistics sets.
 */
static int dp83tg720_get_sset_count(struct phy_device *phydev)
{
	return ARRAY_SIZE(dp83tg720_hw_stats);
}

/**
 * dp83tg720_get_strings - Get the strings for the statistics.
 * @phydev: Pointer to the phy_device structure.
 * @data: Pointer to the buffer where the strings will be stored.
 *
 * Fills the buffer with the strings for the statistics
 */
static void dp83tg720_get_strings(struct phy_device *phydev, u8 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dp83tg720_hw_stats); i++)
		ethtool_puts(&data, dp83tg720_hw_stats[i].string);
}

/**
 * dp83tg720_get_stats - Get the statistics values.
 * @phydev: Pointer to the phy_device structure.
 * @stats: Pointer to the ethtool_stats structure.
 * @data: Pointer to the buffer where the statistics values will be stored.
 */
static void dp83tg720_get_stats(struct phy_device *phydev,
				struct ethtool_stats *stats, u64 *data)
{
	int i, j = 0;

	dp83tg720_cache_reg_values(phydev);

	for (i = 0; i < ARRAY_SIZE(dp83tg720_hw_stats); i++) {
		data[j] = dp83tg720_extract_stat_value(phydev, i);
		j++;
	}
}

/**
 * dp83tg720_update_stats - Update the statistics values.
 * @phydev: Pointer to the phy_device structure.
 *
 * Updates the statistics values.
 */
static void dp83tg720_update_stats(struct phy_device *phydev)
{
	int i;

	dp83tg720_cache_reg_values(phydev);

	for (i = 0; i < ARRAY_SIZE(dp83tg720_hw_stats); i++)
		dp83tg720_extract_stat_value(phydev, i);
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
	 * "DP83TG720S-Q1: Configuring for Open Alliance Specification
	 * Compliance (Rev. B)" application note.
	 * Most of the registers are not documented. Some of register names
	 * are guessed by comparing the register offsets with the DP83TD510E.
	 */

	/* Force master link down */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2,
			       DP83TG720S_TDR_MASTER_LINK_DOWN, 0x0400);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_TDR_CFG2,
			    0xa008);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_TDR_CFG3,
			    0x0928);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_TDR_CFG4,
			    0x0004);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_UNKNOWN_0405,
			    0x6400);
	if (ret)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_UNKNOWN_083F,
			    0x3003);
	if (ret)
		return ret;

	/* Start the TDR */
	ret = phy_set_bits_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_TDR_CFG,
			       DP83TG720S_TDR_START);
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
	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_TDR_CFG);
	if (ret < 0)
		return ret;

	/* Check if the TDR test is done */
	if (!(ret & DP83TG720S_TDR_DONE))
		return 0;

	/* Check for TDR test failure */
	if (!(ret & DP83TG720S_TDR_FAIL)) {
		int location;

		/* Read fault status */
		ret = phy_read_mmd(phydev, MDIO_MMD_VEND2,
				   DP83TG720S_TDR_FAULT_STATUS);
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

	dp83tg720_update_stats(phydev);
	phydev->pause = 0;
	phydev->asym_pause = 0;

	/* Most of Clause 45 registers are not present, so we can't use
	 * genphy_c45_read_status() here.
	 */
	phy_sts = phy_read(phydev, DP83TG720S_MII_REG_10);
	phydev->link = !!(phy_sts & DP83TG720S_LINK_STATUS);
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

	ret = phy_read_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_SQI_REG_1);
	if (ret < 0)
		return ret;

	return FIELD_GET(DP83TG720S_SQI_OUT, ret);
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
		rgmii_delay = DP83TG720S_RGMII_RX_CLK_SEL |
				DP83TG720S_RGMII_TX_CLK_SEL;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		rgmii_delay = DP83TG720S_RGMII_RX_CLK_SEL;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		rgmii_delay = DP83TG720S_RGMII_TX_CLK_SEL;
		break;
	default:
		return 0;
	}

	rgmii_delay_mask = DP83TG720S_RGMII_RX_CLK_SEL |
		DP83TG720S_RGMII_TX_CLK_SEL;

	return phy_modify_mmd(phydev, MDIO_MMD_VEND2,
			      DP83TG720S_RGMII_DELAY_CTRL, rgmii_delay_mask,
			      rgmii_delay);
}

static int dp83tg720_config_init(struct phy_device *phydev)
{
	int ret;

	/* Software Restart is not enough to recover from a link failure.
	 * Using Hardware Reset instead.
	 */
	ret = phy_write(phydev, DP83TG720S_PHY_RESET, DP83TG720S_HW_RESET);
	if (ret)
		return ret;

	/* Wait until MDC can be used again.
	 * The wait value of one 1ms is documented in "DP83TG720S-Q1 1000BASE-T1
	 * Automotive Ethernet PHY with SGMII and RGMII" datasheet.
	 */
	usleep_range(1000, 2000);

	if (phy_interface_is_rgmii(phydev)) {
		ret = dp83tg720_config_rgmii_delay(phydev);
		if (ret)
			return ret;
	}

	/* In case the PHY is bootstrapped in managed mode, we need to
	 * wake it.
	 */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, DP83TG720S_LPS_CFG3,
			    DP83TG720S_LPS_CFG3_PWR_MODE_0);
	if (ret)
		return ret;

	/* Make role configuration visible for ethtool on init and after
	 * rest.
	 */
	return genphy_c45_pma_baset1_read_master_slave(phydev);
}

static int dp83tg720_probe(struct phy_device *phydev)
{
	struct dp83tg720_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	return 0;
}

static struct phy_driver dp83tg720_driver[] = {
{
	PHY_ID_MATCH_MODEL(DP83TG720S_PHY_ID),
	.name		= "TI DP83TG720S",

	.flags          = PHY_POLL_CABLE_TEST,
	.probe		= dp83tg720_probe,
	.config_aneg	= dp83tg720_config_aneg,
	.read_status	= dp83tg720_read_status,
	.get_features	= genphy_c45_pma_read_ext_abilities,
	.config_init	= dp83tg720_config_init,
	.get_sqi	= dp83tg720_get_sqi,
	.get_sqi_max	= dp83tg720_get_sqi_max,
	.cable_test_start = dp83tg720_cable_test_start,
	.cable_test_get_status = dp83tg720_cable_test_get_status,
	.get_sset_count = dp83tg720_get_sset_count,
	.get_strings	= dp83tg720_get_strings,
	.get_stats	= dp83tg720_get_stats,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
} };
module_phy_driver(dp83tg720_driver);

static struct mdio_device_id __maybe_unused dp83tg720_tbl[] = {
	{ PHY_ID_MATCH_MODEL(DP83TG720S_PHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, dp83tg720_tbl);

MODULE_DESCRIPTION("Texas Instruments DP83TG720S PHY driver");
MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
MODULE_LICENSE("GPL");
