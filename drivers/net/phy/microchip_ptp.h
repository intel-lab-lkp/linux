/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2024 Microchip Technology
 */

#ifndef _MICROCHIP_PTP_H
#define _MICROCHIP_PTP_H

#if IS_ENABLED(CONFIG_MICROCHIP_PHYPTP)

#include <linux/ptp_clock_kernel.h>
#include <linux/ptp_clock.h>
#include <linux/ptp_classify.h>
#include <linux/net_tstamp.h>
#include <linux/mii.h>
#include <linux/phy.h>

#define MCHP_PTP_CMD_CTL(b)			((b) + 0x0)
#define MCHP_PTP_CMD_CTL_LTC_STEP_NSEC		BIT(6)
#define MCHP_PTP_CMD_CTL_LTC_STEP_SEC		BIT(5)
#define MCHP_PTP_CMD_CTL_CLOCK_LOAD		BIT(4)
#define MCHP_PTP_CMD_CTL_CLOCK_READ		BIT(3)
#define MCHP_PTP_CMD_CTL_EN			BIT(1)
#define MCHP_PTP_CMD_CTL_DIS			BIT(0)

#define MCHP_PTP_REF_CLK_CFG(b)			((b) + 0x2)
#define MCHP_PTP_REF_CLK_SRC_250MHZ		0x0
#define MCHP_PTP_REF_CLK_PERIOD_OVERRIDE	BIT(9)
#define MCHP_PTP_REF_CLK_PERIOD			4
#define MCHP_PTP_REF_CLK_CFG_SET	(MCHP_PTP_REF_CLK_SRC_250MHZ |\
					 MCHP_PTP_REF_CLK_PERIOD_OVERRIDE |\
					 MCHP_PTP_REF_CLK_PERIOD)

#define MCHP_PTP_LTC_SEC_HI(b)			((b) + 0x5)
#define MCHP_PTP_LTC_SEC_MID(b)			((b) + 0x6)
#define MCHP_PTP_LTC_SEC_LO(b)			((b) + 0x7)
#define MCHP_PTP_LTC_NS_HI(b)			((b) + 0x8)
#define MCHP_PTP_LTC_NS_LO(b)			((b) + 0x9)
#define MCHP_PTP_LTC_RATE_ADJ_HI(b)		((b) + 0xc)
#define MCHP_PTP_LTC_RATE_ADJ_HI_DIR		BIT(15)
#define MCHP_PTP_LTC_RATE_ADJ_LO(b)		((b) + 0xd)
#define MCHP_PTP_LTC_STEP_ADJ_HI(b)		((b) + 0x12)
#define MCHP_PTP_LTC_STEP_ADJ_HI_DIR		BIT(15)
#define MCHP_PTP_LTC_STEP_ADJ_LO(b)		((b) + 0x13)
#define MCHP_PTP_LTC_READ_SEC_HI(b)		((b) + 0x29)
#define MCHP_PTP_LTC_READ_SEC_MID(b)		((b) + 0x2a)
#define MCHP_PTP_LTC_READ_SEC_LO(b)		((b) + 0x2b)
#define MCHP_PTP_LTC_READ_NS_HI(b)		((b) + 0x2c)
#define MCHP_PTP_LTC_READ_NS_LO(b)		((b) + 0x2d)
#define MCHP_PTP_OP_MODE(b)			((b) + 0x41)
#define MCHP_PTP_OP_MODE_DIS			0
#define MCHP_PTP_OP_MODE_STANDALONE		1
#define MCHP_PTP_LATENCY_CORRECTION_CTL(b)	((b) + 0x44)
#define MCHP_PTP_PREDICTOR_EN			BIT(6)
#define MCHP_PTP_TX_PRED_DIS			BIT(1)
#define MCHP_PTP_RX_PRED_DIS			BIT(0)
#define MCHP_PTP_LATENCY_SETTING		(MCHP_PTP_PREDICTOR_EN | \
						 MCHP_PTP_TX_PRED_DIS | \
						 MCHP_PTP_RX_PRED_DIS)

#define MCHP_PTP_INT_EN(b)			((b) + 0x0)
#define MCHP_PTP_INT_STS(b)			((b) + 0x01)
#define MCHP_PTP_INT_TX_TS_OVRFL_EN		BIT(3)
#define MCHP_PTP_INT_TX_TS_EN			BIT(2)
#define MCHP_PTP_INT_RX_TS_OVRFL_EN		BIT(1)
#define MCHP_PTP_INT_RX_TS_EN			BIT(0)
#define MCHP_PTP_INT_ALL_MSK		(MCHP_PTP_INT_TX_TS_OVRFL_EN | \
					 MCHP_PTP_INT_TX_TS_EN | \
					 MCHP_PTP_INT_RX_TS_OVRFL_EN |\
					 MCHP_PTP_INT_RX_TS_EN)

#define MCHP_PTP_CAP_INFO(b)			((b) + 0x2e)
#define MCHP_PTP_TX_TS_CNT(v)			(((v) & GENMASK(11, 8)) >> 8)
#define MCHP_PTP_RX_TS_CNT(v)			((v) & GENMASK(3, 0))

#define MCHP_PTP_RX_PARSE_CONFIG(b)		((b) + 0x42)
#define MCHP_PTP_RX_PARSE_L2_ADDR_EN(b)		((b) + 0x44)
#define MCHP_PTP_RX_PARSE_IPV4_ADDR_EN(b)	((b) + 0x45)

#define MCHP_PTP_RX_TIMESTAMP_CONFIG(b)		((b) + 0x4e)
#define MCHP_PTP_RX_TIMESTAMP_CONFIG_PTP_FCS_DIS BIT(0)

#define MCHP_PTP_RX_VERSION(b)			((b) + 0x48)
#define MCHP_PTP_RX_TIMESTAMP_EN(b)		((b) + 0x4d)

#define MCHP_PTP_RX_INGRESS_NS_HI(b)		((b) + 0x54)
#define MCHP_PTP_RX_INGRESS_NS_HI_TS_VALID	BIT(15)

#define MCHP_PTP_RX_INGRESS_NS_LO(b)		((b) + 0x55)
#define MCHP_PTP_RX_INGRESS_SEC_HI(b)		((b) + 0x56)
#define MCHP_PTP_RX_INGRESS_SEC_LO(b)		((b) + 0x57)
#define MCHP_PTP_RX_MSG_HEADER2(b)		((b) + 0x59)

#define MCHP_PTP_TX_PARSE_CONFIG(b)		((b) + 0x82)
#define MCHP_PTP_PARSE_CONFIG_LAYER2_EN		BIT(0)
#define MCHP_PTP_PARSE_CONFIG_IPV4_EN		BIT(1)
#define MCHP_PTP_PARSE_CONFIG_IPV6_EN		BIT(2)

#define MCHP_PTP_TX_PARSE_L2_ADDR_EN(b)		((b) + 0x84)
#define MCHP_PTP_TX_PARSE_IPV4_ADDR_EN(b)	((b) + 0x85)

#define MCHP_PTP_TX_VERSION(b)			((b) + 0x88)
#define MCHP_PTP_MAX_VERSION(x)			(((x) & GENMASK(7, 0)) << 8)
#define MCHP_PTP_MIN_VERSION(x)			((x) & GENMASK(7, 0))

#define MCHP_PTP_TX_TIMESTAMP_EN(b)		((b) + 0x8d)
#define MCHP_PTP_TIMESTAMP_EN_SYNC		BIT(0)
#define MCHP_PTP_TIMESTAMP_EN_DREQ		BIT(1)
#define MCHP_PTP_TIMESTAMP_EN_PDREQ		BIT(2)
#define MCHP_PTP_TIMESTAMP_EN_PDRES		BIT(3)
#define MCHP_PTP_TIMESTAMP_EN_ALL		(MCHP_PTP_TIMESTAMP_EN_SYNC |\
						 MCHP_PTP_TIMESTAMP_EN_DREQ |\
						 MCHP_PTP_TIMESTAMP_EN_PDREQ |\
						 MCHP_PTP_TIMESTAMP_EN_PDRES)

#define MCHP_PTP_TX_TIMESTAMP_CONFIG(b)		((b) + 0x8e)
#define MCHP_PTP_TX_TIMESTAMP_CONFIG_PTP_FCS_DIS BIT(0)

#define MCHP_PTP_TX_MOD(b)			((b) + 0x8f)
#define MCHP_PTP_TX_MOD_PTP_SYNC_TS_INSERT	BIT(12)
#define MCHP_PTP_TX_MOD_PTP_FU_TS_INSERT	BIT(11)

#define MCHP_PTP_TX_EGRESS_NS_HI(b)		((b) + 0x94)
#define MCHP_PTP_TX_EGRESS_NS_HI_TS_VALID	BIT(15)

#define MCHP_PTP_TX_EGRESS_NS_LO(b)		((b) + 0x95)
#define MCHP_PTP_TX_EGRESS_SEC_HI(b)		((b) + 0x96)
#define MCHP_PTP_TX_EGRESS_SEC_LO(b)		((b) + 0x97)
#define MCHP_PTP_TX_MSG_HEADER2(b)		((b) + 0x99)

#define MCHP_PTP_TSU_GEN_CONFIG(b)		((b) + 0xc0)
#define MCHP_PTP_TSU_GEN_CFG_TSU_EN		BIT(0)

#define MCHP_PTP_TSU_HARD_RESET(b)		((b) + 0xc1)
#define MCHP_PTP_TSU_HARDRESET			BIT(0)

/* Represents 1ppm adjustment in 2^32 format with
 * each nsec contains 4 clock cycles in 250MHz.
 * The value is calculated as following: (1/1000000)/((2^-32)/4)
 */
#define MCHP_PTP_1PPM_FORMAT			17179
#define MCHP_PTP_FIFO_SIZE			8
#define MCHP_PTP_MAX_ADJ				31249999

#define BASE_CLK(p)		((p)->clk_base_addr)
#define BASE_PORT(p)		((p)->port_base_addr)
#define PTP_MMD(p)		((p)->mmd)

enum ptp_fifo_dir {
	PTP_INGRESS_FIFO,
	PTP_EGRESS_FIFO
};

struct mchp_ptp_clock {
	struct mii_timestamper mii_ts;
	struct phy_device *phydev;
	struct ptp_clock *ptp_clock;

	struct sk_buff_head tx_queue;
	struct sk_buff_head rx_queue;
	struct list_head rx_ts_list;

	struct ptp_clock_info caps;

	/* Lock for Rx ts fifo */
	spinlock_t rx_ts_lock;
	int hwts_tx_type;

	enum hwtstamp_rx_filters rx_filter;
	int layer;
	int version;
	u16 port_base_addr;
	u16 clk_base_addr;

	/* Lock for phc */
	struct mutex ptp_lock;
	u8 mmd;
};

struct mchp_ptp_rx_ts {
	struct list_head list;
	u32 seconds;
	u32 nsec;
	u16 seq_id;
};

struct mchp_ptp_clock *mchp_ptp_probe(struct phy_device *phydev, u8 mmd,
				      u16 clk_base, u16 port_base);

int mchp_config_ptp_intr(struct mchp_ptp_clock *ptp_clock,
			 u16 reg, u16 val, bool enable);

irqreturn_t mchp_ptp_handle_interrupt(struct mchp_ptp_clock *ptp_clock);

#else

static inline struct mchp_ptp_clock *mchp_ptp_probe(struct phy_device *phydev,
						    u8 mmd, u16 clk_base,
						    u16 port_base)
{
	return NULL;
}

static inline int mchp_config_ptp_intr(struct mchp_ptp_clock *ptp_clock,
				       u16 reg, u16 val, bool enable)
{
	return 0;
}

static inline irqreturn_t mchp_ptp_handle_interrupt(struct mchp_ptp_clock *ptp_clock)
{
	return IRQ_NONE;
}

#endif //CONFIG_MICROCHIP_PHYPTP

#endif //_MICROCHIP_PTP_H
