// SPDX-License-Identifier: GPL-2.0+

#include <linux/phy.h>
#include <linux/module.h>
#include <linux/ethtool_netlink.h>

#include "qcom.h"

/* ADC threshold */
#define QCA808X_PHY_DEBUG_ADC_THRESHOLD		0x2c80
#define QCA808X_ADC_THRESHOLD_MASK		GENMASK(7, 0)
#define QCA808X_ADC_THRESHOLD_80MV		0
#define QCA808X_ADC_THRESHOLD_100MV		0xf0
#define QCA808X_ADC_THRESHOLD_200MV		0x0f
#define QCA808X_ADC_THRESHOLD_300MV		0xff

/* CLD control */
#define QCA808X_PHY_MMD3_ADDR_CLD_CTRL7		0x8007
#define QCA808X_8023AZ_AFE_CTRL_MASK		GENMASK(8, 4)
#define QCA808X_8023AZ_AFE_EN			0x90

/* AZ control */
#define QCA808X_PHY_MMD3_AZ_TRAINING_CTRL	0x8008
#define QCA808X_MMD3_AZ_TRAINING_VAL		0x1c32

#define QCA808X_PHY_MMD1_MSE_THRESHOLD_20DB	0x8014
#define QCA808X_MSE_THRESHOLD_20DB_VALUE	0x529

#define QCA808X_PHY_MMD1_MSE_THRESHOLD_17DB	0x800E
#define QCA808X_MSE_THRESHOLD_17DB_VALUE	0x341

#define QCA808X_PHY_MMD1_MSE_THRESHOLD_27DB	0x801E
#define QCA808X_MSE_THRESHOLD_27DB_VALUE	0x419

#define QCA808X_PHY_MMD1_MSE_THRESHOLD_28DB	0x8020
#define QCA808X_MSE_THRESHOLD_28DB_VALUE	0x341

#define QCA808X_PHY_MMD7_TOP_OPTION1		0x901c
#define QCA808X_TOP_OPTION1_DATA		0x0

#define QCA808X_PHY_MMD3_DEBUG_1		0xa100
#define QCA808X_MMD3_DEBUG_1_VALUE		0x9203
#define QCA808X_PHY_MMD3_DEBUG_2		0xa101
#define QCA808X_MMD3_DEBUG_2_VALUE		0x48ad
#define QCA808X_PHY_MMD3_DEBUG_3		0xa103
#define QCA808X_MMD3_DEBUG_3_VALUE		0x1698
#define QCA808X_PHY_MMD3_DEBUG_4		0xa105
#define QCA808X_MMD3_DEBUG_4_VALUE		0x8001
#define QCA808X_PHY_MMD3_DEBUG_5		0xa106
#define QCA808X_MMD3_DEBUG_5_VALUE		0x1111
#define QCA808X_PHY_MMD3_DEBUG_6		0xa011
#define QCA808X_MMD3_DEBUG_6_VALUE		0x5f85

/* master/slave seed config */
#define QCA808X_PHY_DEBUG_LOCAL_SEED		9
#define QCA808X_MASTER_SLAVE_SEED_ENABLE	BIT(1)
#define QCA808X_MASTER_SLAVE_SEED_CFG		GENMASK(12, 2)
#define QCA808X_MASTER_SLAVE_SEED_RANGE		0x32

/* Hibernation yields lower power consumpiton in contrast with normal operation mode.
 * when the copper cable is unplugged, the PHY enters into hibernation mode in about 10s.
 */
#define QCA808X_DBG_AN_TEST			0xb
#define QCA808X_HIBERNATION_EN			BIT(15)

#define QCA808X_CDT_ENABLE_TEST			BIT(15)
#define QCA808X_CDT_INTER_CHECK_DIS		BIT(13)
#define QCA808X_CDT_LENGTH_UNIT			BIT(10)

#define QCA808X_MMD3_CDT_STATUS			0x8064
#define QCA808X_MMD3_CDT_DIAG_PAIR_A		0x8065
#define QCA808X_MMD3_CDT_DIAG_PAIR_B		0x8066
#define QCA808X_MMD3_CDT_DIAG_PAIR_C		0x8067
#define QCA808X_MMD3_CDT_DIAG_PAIR_D		0x8068
#define QCA808X_CDT_DIAG_LENGTH			GENMASK(7, 0)

#define QCA808X_CDT_CODE_PAIR_A			GENMASK(15, 12)
#define QCA808X_CDT_CODE_PAIR_B			GENMASK(11, 8)
#define QCA808X_CDT_CODE_PAIR_C			GENMASK(7, 4)
#define QCA808X_CDT_CODE_PAIR_D			GENMASK(3, 0)
#define QCA808X_CDT_STATUS_STAT_FAIL		0
#define QCA808X_CDT_STATUS_STAT_NORMAL		1
#define QCA808X_CDT_STATUS_STAT_OPEN		2
#define QCA808X_CDT_STATUS_STAT_SHORT		3

/* QCA808X 1G chip type */
#define QCA808X_PHY_MMD7_CHIP_TYPE		0x901d
#define QCA808X_PHY_CHIP_TYPE_1G		BIT(0)

#define QCA8081_PHY_SERDES_MMD1_FIFO_CTRL	0x9072
#define QCA8081_PHY_FIFO_RSTN			BIT(11)

#define QCA8081_PHY_ID				0x004dd101

MODULE_DESCRIPTION("Qualcomm QCA808X PHY driver");
MODULE_AUTHOR("Matus Ujhelyi");
MODULE_LICENSE("GPL");

static int qca808x_config_aneg(struct phy_device *phydev)
{
	int phy_ctrl = 0;
	int ret;

	ret = at803x_config_mdix(phydev, phydev->mdix_ctrl);
	if (ret < 0)
		return ret;

	/* Changes of the midx bits are disruptive to the normal operation;
	 * therefore any changes to these registers must be followed by a
	 * software reset to take effect.
	 */
	if (ret == 1) {
		ret = genphy_soft_reset(phydev);
		if (ret < 0)
			return ret;
	}

	/* Do not restart auto-negotiation by setting ret to 0 defautly,
	 * when calling __genphy_config_aneg later.
	 */
	ret = 0;

	/* The reg MII_BMCR also needs to be configured for force mode, the
	 * genphy_config_aneg is also needed.
	 */
	if (phydev->autoneg == AUTONEG_DISABLE)
		genphy_c45_pma_setup_forced(phydev);

	if (linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->advertising))
		phy_ctrl = MDIO_AN_10GBT_CTRL_ADV2_5G;

	ret = phy_modify_mmd_changed(phydev, MDIO_MMD_AN, MDIO_AN_10GBT_CTRL,
				     MDIO_AN_10GBT_CTRL_ADV2_5G, phy_ctrl);
	if (ret < 0)
		return ret;

	return __genphy_config_aneg(phydev, ret);
}

static int qca808x_phy_fast_retrain_config(struct phy_device *phydev)
{
	int ret;

	/* Enable fast retrain */
	ret = genphy_c45_fast_retrain(phydev, true);
	if (ret)
		return ret;

	phy_write_mmd(phydev, MDIO_MMD_AN, QCA808X_PHY_MMD7_TOP_OPTION1,
		      QCA808X_TOP_OPTION1_DATA);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, QCA808X_PHY_MMD1_MSE_THRESHOLD_20DB,
		      QCA808X_MSE_THRESHOLD_20DB_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, QCA808X_PHY_MMD1_MSE_THRESHOLD_17DB,
		      QCA808X_MSE_THRESHOLD_17DB_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, QCA808X_PHY_MMD1_MSE_THRESHOLD_27DB,
		      QCA808X_MSE_THRESHOLD_27DB_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, QCA808X_PHY_MMD1_MSE_THRESHOLD_28DB,
		      QCA808X_MSE_THRESHOLD_28DB_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_1,
		      QCA808X_MMD3_DEBUG_1_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_4,
		      QCA808X_MMD3_DEBUG_4_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_5,
		      QCA808X_MMD3_DEBUG_5_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_3,
		      QCA808X_MMD3_DEBUG_3_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_6,
		      QCA808X_MMD3_DEBUG_6_VALUE);
	phy_write_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_DEBUG_2,
		      QCA808X_MMD3_DEBUG_2_VALUE);

	return 0;
}

static int qca808x_phy_ms_seed_enable(struct phy_device *phydev, bool enable)
{
	u16 seed_value;

	if (!enable)
		return at803x_debug_reg_mask(phydev, QCA808X_PHY_DEBUG_LOCAL_SEED,
				QCA808X_MASTER_SLAVE_SEED_ENABLE, 0);

	seed_value = get_random_u32_below(QCA808X_MASTER_SLAVE_SEED_RANGE);
	return at803x_debug_reg_mask(phydev, QCA808X_PHY_DEBUG_LOCAL_SEED,
			QCA808X_MASTER_SLAVE_SEED_CFG | QCA808X_MASTER_SLAVE_SEED_ENABLE,
			FIELD_PREP(QCA808X_MASTER_SLAVE_SEED_CFG, seed_value) |
			QCA808X_MASTER_SLAVE_SEED_ENABLE);
}

static bool qca808x_is_prefer_master(struct phy_device *phydev)
{
	return (phydev->master_slave_get == MASTER_SLAVE_CFG_MASTER_FORCE) ||
		(phydev->master_slave_get == MASTER_SLAVE_CFG_MASTER_PREFERRED);
}

static bool qca808x_has_fast_retrain_or_slave_seed(struct phy_device *phydev)
{
	return linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->supported);
}

static int qca808x_config_init(struct phy_device *phydev)
{
	int ret;

	/* Active adc&vga on 802.3az for the link 1000M and 100M */
	ret = phy_modify_mmd(phydev, MDIO_MMD_PCS, QCA808X_PHY_MMD3_ADDR_CLD_CTRL7,
			     QCA808X_8023AZ_AFE_CTRL_MASK, QCA808X_8023AZ_AFE_EN);
	if (ret)
		return ret;

	/* Adjust the threshold on 802.3az for the link 1000M */
	ret = phy_write_mmd(phydev, MDIO_MMD_PCS,
			    QCA808X_PHY_MMD3_AZ_TRAINING_CTRL,
			    QCA808X_MMD3_AZ_TRAINING_VAL);
	if (ret)
		return ret;

	if (qca808x_has_fast_retrain_or_slave_seed(phydev)) {
		/* Config the fast retrain for the link 2500M */
		ret = qca808x_phy_fast_retrain_config(phydev);
		if (ret)
			return ret;

		ret = genphy_read_master_slave(phydev);
		if (ret < 0)
			return ret;

		if (!qca808x_is_prefer_master(phydev)) {
			/* Enable seed and configure lower ramdom seed to make phy
			 * linked as slave mode.
			 */
			ret = qca808x_phy_ms_seed_enable(phydev, true);
			if (ret)
				return ret;
		}
	}

	/* Configure adc threshold as 100mv for the link 10M */
	return at803x_debug_reg_mask(phydev, QCA808X_PHY_DEBUG_ADC_THRESHOLD,
				     QCA808X_ADC_THRESHOLD_MASK,
				     QCA808X_ADC_THRESHOLD_100MV);
}

static int qca808x_read_status(struct phy_device *phydev)
{
	struct at803x_ss_mask ss_mask = { 0 };
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_10GBT_STAT);
	if (ret < 0)
		return ret;

	linkmode_mod_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->lp_advertising,
			 ret & MDIO_AN_10GBT_STAT_LP2_5G);

	ret = genphy_read_status(phydev);
	if (ret)
		return ret;

	/* qca8081 takes the different bits for speed value from at803x */
	ss_mask.speed_mask = QCA808X_SS_SPEED_MASK;
	ss_mask.speed_shift = __bf_shf(QCA808X_SS_SPEED_MASK);
	ret = at803x_read_specific_status(phydev, ss_mask);
	if (ret < 0)
		return ret;

	if (phydev->link) {
		if (phydev->speed == SPEED_2500)
			phydev->interface = PHY_INTERFACE_MODE_2500BASEX;
		else
			phydev->interface = PHY_INTERFACE_MODE_SGMII;
	} else {
		/* generate seed as a lower random value to make PHY linked as SLAVE easily,
		 * except for master/slave configuration fault detected or the master mode
		 * preferred.
		 *
		 * the reason for not putting this code into the function link_change_notify is
		 * the corner case where the link partner is also the qca8081 PHY and the seed
		 * value is configured as the same value, the link can't be up and no link change
		 * occurs.
		 */
		if (qca808x_has_fast_retrain_or_slave_seed(phydev)) {
			if (phydev->master_slave_state == MASTER_SLAVE_STATE_ERR ||
			    qca808x_is_prefer_master(phydev)) {
				qca808x_phy_ms_seed_enable(phydev, false);
			} else {
				qca808x_phy_ms_seed_enable(phydev, true);
			}
		}
	}

	return 0;
}

static int qca808x_soft_reset(struct phy_device *phydev)
{
	int ret;

	ret = genphy_soft_reset(phydev);
	if (ret < 0)
		return ret;

	if (qca808x_has_fast_retrain_or_slave_seed(phydev))
		ret = qca808x_phy_ms_seed_enable(phydev, true);

	return ret;
}

static bool qca808x_cdt_fault_length_valid(int cdt_code)
{
	switch (cdt_code) {
	case QCA808X_CDT_STATUS_STAT_SHORT:
	case QCA808X_CDT_STATUS_STAT_OPEN:
		return true;
	default:
		return false;
	}
}

static int qca808x_cable_test_result_trans(int cdt_code)
{
	switch (cdt_code) {
	case QCA808X_CDT_STATUS_STAT_NORMAL:
		return ETHTOOL_A_CABLE_RESULT_CODE_OK;
	case QCA808X_CDT_STATUS_STAT_SHORT:
		return ETHTOOL_A_CABLE_RESULT_CODE_SAME_SHORT;
	case QCA808X_CDT_STATUS_STAT_OPEN:
		return ETHTOOL_A_CABLE_RESULT_CODE_OPEN;
	case QCA808X_CDT_STATUS_STAT_FAIL:
	default:
		return ETHTOOL_A_CABLE_RESULT_CODE_UNSPEC;
	}
}

static int qca808x_cdt_fault_length(struct phy_device *phydev, int pair)
{
	int val;
	u32 cdt_length_reg = 0;

	switch (pair) {
	case ETHTOOL_A_CABLE_PAIR_A:
		cdt_length_reg = QCA808X_MMD3_CDT_DIAG_PAIR_A;
		break;
	case ETHTOOL_A_CABLE_PAIR_B:
		cdt_length_reg = QCA808X_MMD3_CDT_DIAG_PAIR_B;
		break;
	case ETHTOOL_A_CABLE_PAIR_C:
		cdt_length_reg = QCA808X_MMD3_CDT_DIAG_PAIR_C;
		break;
	case ETHTOOL_A_CABLE_PAIR_D:
		cdt_length_reg = QCA808X_MMD3_CDT_DIAG_PAIR_D;
		break;
	default:
		return -EINVAL;
	}

	val = phy_read_mmd(phydev, MDIO_MMD_PCS, cdt_length_reg);
	if (val < 0)
		return val;

	return (FIELD_GET(QCA808X_CDT_DIAG_LENGTH, val) * 824) / 10;
}

static int qca808x_cable_test_start(struct phy_device *phydev)
{
	int ret;

	/* perform CDT with the following configs:
	 * 1. disable hibernation.
	 * 2. force PHY working in MDI mode.
	 * 3. for PHY working in 1000BaseT.
	 * 4. configure the threshold.
	 */

	ret = at803x_debug_reg_mask(phydev, QCA808X_DBG_AN_TEST, QCA808X_HIBERNATION_EN, 0);
	if (ret < 0)
		return ret;

	ret = at803x_config_mdix(phydev, ETH_TP_MDI);
	if (ret < 0)
		return ret;

	/* Force 1000base-T needs to configure PMA/PMD and MII_BMCR */
	phydev->duplex = DUPLEX_FULL;
	phydev->speed = SPEED_1000;
	ret = genphy_c45_pma_setup_forced(phydev);
	if (ret < 0)
		return ret;

	ret = genphy_setup_forced(phydev);
	if (ret < 0)
		return ret;

	/* configure the thresholds for open, short, pair ok test */
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x8074, 0xc040);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x8076, 0xc040);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x8077, 0xa060);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x8078, 0xc050);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x807a, 0xc060);
	phy_write_mmd(phydev, MDIO_MMD_PCS, 0x807e, 0xb060);

	return 0;
}

static int qca808x_cdt_start(struct phy_device *phydev)
{
	u16 cdt;

	/* qca8081 takes the different bit 15 to enable CDT test */
	cdt = QCA808X_CDT_ENABLE_TEST |
	      QCA808X_CDT_LENGTH_UNIT |
	      QCA808X_CDT_INTER_CHECK_DIS;

	return phy_write(phydev, AT803X_CDT, cdt);
}

static int qca808x_cdt_wait_for_completition(struct phy_device *phydev)
{
	int val, ret;

	/* One test run takes about 25ms */
	ret = phy_read_poll_timeout(phydev, AT803X_CDT, val,
				    !(val & QCA808X_CDT_ENABLE_TEST),
				    30000, 100000, true);

	return ret < 0 ? ret : 0;
}

static int qca808x_cable_test_get_status(struct phy_device *phydev, bool *finished)
{
	int ret, val;
	int pair_a, pair_b, pair_c, pair_d;

	*finished = false;

	ret = qca808x_cdt_start(phydev);
	if (ret)
		return ret;

	ret = qca808x_cdt_wait_for_completition(phydev);
	if (ret)
		return ret;

	val = phy_read_mmd(phydev, MDIO_MMD_PCS, QCA808X_MMD3_CDT_STATUS);
	if (val < 0)
		return val;

	pair_a = FIELD_GET(QCA808X_CDT_CODE_PAIR_A, val);
	pair_b = FIELD_GET(QCA808X_CDT_CODE_PAIR_B, val);
	pair_c = FIELD_GET(QCA808X_CDT_CODE_PAIR_C, val);
	pair_d = FIELD_GET(QCA808X_CDT_CODE_PAIR_D, val);

	ethnl_cable_test_result(phydev, ETHTOOL_A_CABLE_PAIR_A,
				qca808x_cable_test_result_trans(pair_a));
	ethnl_cable_test_result(phydev, ETHTOOL_A_CABLE_PAIR_B,
				qca808x_cable_test_result_trans(pair_b));
	ethnl_cable_test_result(phydev, ETHTOOL_A_CABLE_PAIR_C,
				qca808x_cable_test_result_trans(pair_c));
	ethnl_cable_test_result(phydev, ETHTOOL_A_CABLE_PAIR_D,
				qca808x_cable_test_result_trans(pair_d));

	if (qca808x_cdt_fault_length_valid(pair_a))
		ethnl_cable_test_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_A,
				qca808x_cdt_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_A));
	if (qca808x_cdt_fault_length_valid(pair_b))
		ethnl_cable_test_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_B,
				qca808x_cdt_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_B));
	if (qca808x_cdt_fault_length_valid(pair_c))
		ethnl_cable_test_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_C,
				qca808x_cdt_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_C));
	if (qca808x_cdt_fault_length_valid(pair_d))
		ethnl_cable_test_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_D,
				qca808x_cdt_fault_length(phydev, ETHTOOL_A_CABLE_PAIR_D));

	*finished = true;

	return 0;
}

static int qca808x_get_features(struct phy_device *phydev)
{
	int ret;

	ret = genphy_c45_pma_read_abilities(phydev);
	if (ret)
		return ret;

	/* The autoneg ability is not existed in bit3 of MMD7.1,
	 * but it is supported by qca808x PHY, so we add it here
	 * manually.
	 */
	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT, phydev->supported);

	/* As for the qca8081 1G version chip, the 2500baseT ability is also
	 * existed in the bit0 of MMD1.21, we need to remove it manually if
	 * it is the qca8081 1G chip according to the bit0 of MMD7.0x901d.
	 */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, QCA808X_PHY_MMD7_CHIP_TYPE);
	if (ret < 0)
		return ret;

	if (QCA808X_PHY_CHIP_TYPE_1G & ret)
		linkmode_clear_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, phydev->supported);

	return 0;
}

static void qca808x_link_change_notify(struct phy_device *phydev)
{
	/* Assert interface sgmii fifo on link down, deassert it on link up,
	 * the interface device address is always phy address added by 1.
	 */
	mdiobus_c45_modify_changed(phydev->mdio.bus, phydev->mdio.addr + 1,
				   MDIO_MMD_PMAPMD, QCA8081_PHY_SERDES_MMD1_FIFO_CTRL,
				   QCA8081_PHY_FIFO_RSTN,
				   phydev->link ? QCA8081_PHY_FIFO_RSTN : 0);
}

static struct phy_driver qca808x_driver[] = {
{
	/* Qualcomm QCA8081 */
	PHY_ID_MATCH_EXACT(QCA8081_PHY_ID),
	.name			= "Qualcomm QCA8081",
	.flags			= PHY_POLL_CABLE_TEST,
	.config_intr		= at803x_config_intr,
	.handle_interrupt	= at803x_handle_interrupt,
	.get_tunable		= at803x_get_tunable,
	.set_tunable		= at803x_set_tunable,
	.set_wol		= at803x_set_wol,
	.get_wol		= at803x_get_wol,
	.get_features		= qca808x_get_features,
	.config_aneg		= qca808x_config_aneg,
	.suspend		= genphy_suspend,
	.resume			= genphy_resume,
	.read_status		= qca808x_read_status,
	.config_init		= qca808x_config_init,
	.soft_reset		= qca808x_soft_reset,
	.cable_test_start	= qca808x_cable_test_start,
	.cable_test_get_status	= qca808x_cable_test_get_status,
	.link_change_notify	= qca808x_link_change_notify,
}, };

module_phy_driver(qca808x_driver);

static struct mdio_device_id __maybe_unused qca808x_tbl[] = {
	{ PHY_ID_MATCH_EXACT(QCA8081_PHY_ID) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, qca808x_tbl);
