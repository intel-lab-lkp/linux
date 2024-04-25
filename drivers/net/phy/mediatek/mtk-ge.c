// SPDX-License-Identifier: GPL-2.0+
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/phy.h>

#include "mtk.h"

#define MTK_PHY_PAGE_EXTENDED_1			0x0001
#define MTK_PHY_AUX_CTRL_AND_STATUS		0x14
#define   MTK_PHY_ENABLE_DOWNSHIFT		BIT(4)

#define MTK_PHY_PAGE_EXTENDED_2			0x0002
#define MTK_PHY_PAGE_EXTENDED_3			0x0003
#define MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG11	0x11

#define MTK_PHY_PAGE_EXTENDED_2A30		0x2a30

/* Registers on Token Ring debug nodes */
/* ch_addr = 0x1, node_addr = 0xf, data_addr = 0x17 */
#define SLAVE_DSP_READY_TIME_MASK		GENMASK(22, 15)

/* Registers on MDIO_MMD_VEND1 */
#define MTK_PHY_GBE_MODE_TX_DELAY_SEL		0x13
#define MTK_PHY_TEST_MODE_TX_DELAY_SEL		0x14
#define   MTK_TX_DELAY_PAIR_B_MASK		GENMASK(10, 8)
#define   MTK_TX_DELAY_PAIR_D_MASK		GENMASK(2, 0)

#define MTK_PHY_MCC_CTRL_AND_TX_POWER_CTRL	0xa6
#define   MTK_MCC_NEARECHO_OFFSET_MASK		GENMASK(15, 8)

#define MTK_PHY_RXADC_CTRL_RG7			0xc6
#define   MTK_PHY_DA_AD_BUF_BIAS_LP_MASK	GENMASK(9, 8)

#define MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG123	0x123
#define   MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK	GENMASK(15, 8)
#define   MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK	GENMASK(7, 0)

static void mtk_gephy_config_init(struct phy_device *phydev)
{
	/* Enable HW auto downshift */
	phy_modify_paged(phydev, MTK_PHY_PAGE_EXTENDED_1, MTK_PHY_AUX_CTRL_AND_STATUS,
			 0, MTK_PHY_ENABLE_DOWNSHIFT);

	/* Increase SlvDPSready time */
	tr_modify(phydev, 0x1, 0xf, 0x17, SLAVE_DSP_READY_TIME_MASK,
		  FIELD_PREP(SLAVE_DSP_READY_TIME_MASK, 0x5e));

	/* Adjust 100_mse_threshold */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1,
		       MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG123,
		       MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK |
		       MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK,
		       FIELD_PREP(MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK,
				  0xff) |
		       FIELD_PREP(MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK,
				  0xff));

	/* If echo time is narrower than 0x3, it will be regarded as noise */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_MCC_CTRL_AND_TX_POWER_CTRL,
		       MTK_MCC_NEARECHO_OFFSET_MASK,
		       FIELD_PREP(MTK_MCC_NEARECHO_OFFSET_MASK, 0x3));
}

static int mt7530_phy_config_init(struct phy_device *phydev)
{
	mtk_gephy_config_init(phydev);

	/* Increase post_update_timer */
	phy_write_paged(phydev, MTK_PHY_PAGE_EXTENDED_3,
			MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG11, 0x4b);

	return 0;
}

static int mt7531_phy_config_init(struct phy_device *phydev)
{
	mtk_gephy_config_init(phydev);

	/* PHY link down power saving enable */
	phy_set_bits(phydev, 0x17, BIT(4));
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RXADC_CTRL_RG7,
		       MTK_PHY_DA_AD_BUF_BIAS_LP_MASK,
		       FIELD_PREP(MTK_PHY_DA_AD_BUF_BIAS_LP_MASK, 0x3));

	/* Set TX Pair delay selection */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_GBE_MODE_TX_DELAY_SEL,
		       MTK_TX_DELAY_PAIR_B_MASK | MTK_TX_DELAY_PAIR_D_MASK,
		       FIELD_PREP(MTK_TX_DELAY_PAIR_B_MASK, 0x4) |
		       FIELD_PREP(MTK_TX_DELAY_PAIR_D_MASK, 0x4));
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_TEST_MODE_TX_DELAY_SEL,
		       MTK_TX_DELAY_PAIR_B_MASK | MTK_TX_DELAY_PAIR_D_MASK,
		       FIELD_PREP(MTK_TX_DELAY_PAIR_B_MASK, 0x4) |
		       FIELD_PREP(MTK_TX_DELAY_PAIR_D_MASK, 0x4));

	return 0;
}

static int mtk_phy_cl22_read_status(struct phy_device *phydev)
{
	int err, old_link = phydev->link;

	/* Update the link, but return if there was an error */
	err = genphy_update_link(phydev);
	if (err)
		return err;

	/* why bother the PHY if nothing can have changed */
	if (phydev->autoneg == AUTONEG_ENABLE && old_link && phydev->link)
		return 0;

	phydev->master_slave_get = MASTER_SLAVE_CFG_UNSUPPORTED;
	phydev->master_slave_state = MASTER_SLAVE_STATE_UNSUPPORTED;
	phydev->speed = SPEED_UNKNOWN;
	phydev->duplex = DUPLEX_UNKNOWN;
	phydev->pause = 0;
	phydev->asym_pause = 0;

	if (phydev->is_gigabit_capable) {
		err = genphy_read_master_slave(phydev);
		if (err < 0)
			return err;
		}

	err = genphy_read_lpa(phydev);
	if (err < 0)
		return err;

	if (phydev->autoneg == AUTONEG_ENABLE) {
		if (phydev->autoneg_complete) {
			phy_resolve_aneg_linkmode(phydev);
		} else if (!linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
					      phydev->advertising) &&
			   linkmode_test_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
					     phydev->advertising)) {
			extend_an_new_lp_cnt_limit(phydev);
		}
	} else if (phydev->autoneg == AUTONEG_DISABLE) {
		err = genphy_read_status_fixed(phydev);
		if (err < 0)
			return err;
	}

	return 0;
}

static struct phy_driver mtk_gephy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(0x03a29412),
		.name		= "MediaTek MT7530 PHY",
		.config_init	= mt7530_phy_config_init,
		.read_status	= mtk_phy_cl22_read_status,
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
		PHY_ID_MATCH_EXACT(0x03a29441),
		.name		= "MediaTek MT7531 PHY",
		.config_init	= mt7531_phy_config_init,
		.read_status	= mtk_phy_cl22_read_status,
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
};

module_phy_driver(mtk_gephy_driver);

static struct mdio_device_id __maybe_unused mtk_gephy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(0x03a29441) },
	{ PHY_ID_MATCH_EXACT(0x03a29412) },
	{ }
};

MODULE_DESCRIPTION("MediaTek Gigabit Ethernet PHY driver");
MODULE_AUTHOR("DENG, Qingfang <dqfext@gmail.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, mtk_gephy_tbl);
