/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright Fiona Klute <fiona.klute@gmx.de> */

#ifndef __RTW8703B_H__
#define __RTW8703B_H__

extern const struct rtw_chip_info rtw8703b_hw_spec;

/* phy status parsing */
#define GET_PHY_STAT_AGC_GAIN_A(phy_stat)                                   \
	(le32_get_bits(*((__le32 *)(phy_stat) + 0x00), GENMASK(6, 0)))

#define GET_PHY_STAT_PWDB(phy_stat)                                         \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), GENMASK(7, 0))
#define GET_PHY_STAT_VGA(phy_stat)                                          \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), GENMASK(12, 8))
#define GET_PHY_STAT_LNA_L(phy_stat)                                        \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), GENMASK(15, 13))
/* the high LNA stat bit if 4 bit format is used */
#define GET_PHY_STAT_LNA_H(phy_stat)                                        \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), BIT(23))
#define BIT_LNA_H_MASK BIT(3)
#define BIT_LNA_L_MASK GENMASK(2, 0)

#define GET_PHY_STAT_CFO_TAIL_A(phy_stat)                                   \
	(le32_get_bits(*((__le32 *)(phy_stat) + 0x02), GENMASK(15, 8)))
#define GET_PHY_STAT_RXEVM_A(phy_stat)                                      \
	(le32_get_bits(*((__le32 *)(phy_stat) + 0x03), GENMASK(15, 8)))
#define GET_PHY_STAT_RXSNR_A(phy_stat)                                      \
	(le32_get_bits(*((__le32 *)(phy_stat) + 0x03), GENMASK(31, 24)))

/* Baseband registers */
#define REG_BB_PWR_SAV5_11N 0x0818
/* BIT(11) should be 1 for 8703B *and* 8723D, which means LNA uses 4
 * bit for CCK rates in report, not 3. Vendor driver logs a warning if
 * it's 0, but handles the case.
 *
 * Purpose of other parts of this register is unknown, 8723cs driver
 * code indicates some other chips use certain bits for antenna
 * diversity.
 */
#define REG_BB_AMP 0x0950
#define BIT_MASK_RX_LNA (BIT(11))

/* 0xaXX: 40MHz channel settings */
#define REG_CCK_TXSF2 0x0a24  /* CCK TX filter 2 */
#define REG_CCK_DBG 0x0a28  /* debug port */
#define REG_OFDM0_A_TX_AFE 0x0c84
#define REG_TXIQK_MATRIXB_LSB2_11N 0x0c9c
#define REG_OFDM0_TX_PSD_NOISE 0x0ce4  /* TX pseudo noise weighting */
/* is != 0 when IQK is done */
#define REG_IQK_RDY 0x0e90

/* RF registers */
#define RF_RCK1 0x1E

#define AGG_BURST_NUM 3
#define AGG_BURST_SIZE 0 /* 1K */
#define BIT_MASK_AGG_BURST_NUM (GENMASK(3, 2))
#define BIT_MASK_AGG_BURST_SIZE (GENMASK(5, 4))

#endif /* __RTW8703B_H__ */
