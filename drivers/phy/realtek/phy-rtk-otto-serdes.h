/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek RTL838x, RTL839x, RTL930x & RTL931x SerDes PHY driver
 * Copyright (c) 2025 Markus Stockhausen <markus.stockhausen@gmx.de>
 */

#ifndef _PHY_RTK_OTTO_SERDES_H
#define _PHY_RTK_OTTO_SERDES_H

#define RTSDS_CHIP_ID_MASK			GENMASK(15, 0)
#define RTSDS_MODEL_ID_MASK			GENMASK(31, 16)
#define RTSDS_MODEL_VERSION_MASK		GENMASK(15, 11)

#define RTSDS_COMBOMODE(mode, submode)		(0x10000 | (mode << 8) | submode)
#define RTSDS_MODE(combomode)			((combomode >> 8) & 0xff)
#define RTSDS_SUBMODE(combomode)		(combomode & 0xff)

#define RTSDS_INV_HSO_BIT			BIT(8)
#define RTSDS_INV_HSI_BIT			BIT(9)
#define RTSDS_SOFT_RST_BIT			BIT(6)
#define RTSDS_SDS_EN_RX_BIT			BIT(1)
#define RTSDS_SDS_EN_TX_BIT			BIT(0)
#define RTSDS_RX_SELF_BIT			BIT(9)
#define RTSDS_RX_SELF_10G_BIT			BIT(15)

#define RTSDS_DEBUG_PAGE_MASK			GENMASK_ULL(45, 40)
#define RTSDS_DEBUG_REG_MASK			GENMASK_ULL(39, 32)
#define RTSDS_DEBUG_FIELD_MASK			GENMASK_ULL(31, 16)
#define RTSDS_DEBUG_VAL_MASK			GENMASK_ULL(15, 0)

#define RTSDS_SDS_PAGE				0x00
#define RTSDS_SDS_EXT_PAGE			0x01
#define RTSDS_FIB_PAGE				0x02
#define RTSDS_FIB_EXT_PAGE			0x03
#define RTSDS_ANA_RG_EXT_PAGE			0x09
#define RTSDS_ANA_TG_EXT_PAGE			0x0b

#define RTSDS_FULL_REG_MASK			GENMASK(15, 0)
#define RTSDS_REALTEK_MODE_HSGMII		PHY_INTERFACE_MODE_INTERNAL

#define RTSDS_838X_CFG_FAMILY			0x8380
#define RTSDS_838X_CFG_PAGE_CNT			4
#define RTSDS_838X_CFG_SDS_CNT			6

#define RTSDS_838X_INT_MODE_CTRL_REG		0x005c
#define RTSDS_838X_MODEL_NAME_INFO_REG		0x00d4
#define RTSDS_838X_SDS_MODE_SEL_REG		0x0028

#define RTSDS_838X_SDS_MODE_MASK		GENMASK(4, 0)
#define RTSDS_838X_SDS_MODE_BPOS(sid)		(25 - sid * 5)
#define RTSDS_838X_SDS_SUBMODE_MASK		GENMASK(2, 0)
#define RTSDS_838X_SDS_SUBMODE_BPOS(sid)	((sid - 4) * 3)

#define RTSDS_839X_CFG_FAMILY			0x8390
#define RTSDS_839X_CFG_PAGE_CNT			12
#define RTSDS_839X_CFG_SDS_CNT			14

#define RTSDS_839X_MAC_SDS_IF_CTL_REG(sid)	(0x0008 + ((sid >> 1) & ~3))
#define RTSDS_839X_MODEL_NAME_INFO_REG		0x0ff0

#define RTSDS_839X_SDS_MODE_MASK		GENMASK(3, 0)
#define RTSDS_839X_SDS_MODE_BPOS(sid)		((sid & 7) << 2)
#define RTSDS_839X_SDS_SUBMODE_MASK		GENMASK(15, 12)
#define RTSDS_839X_SDS_RW_BPOS(reg)		((reg << 4) & 0x10)

#define RTSDS_83XX_SDS_CHIP_INFO_EN		0xa0000000
#define RTSDS_83XX_SDS_CHIP_RL_ID_MASK		GENMASK(20, 16)

#define RTSDS_930X_CFG_FAMILY			0x9300
#define RTSDS_930X_CFG_PAGE_CNT			64
#define RTSDS_930X_CFG_SDS_CNT			12

#define RTSDS_930X_SDS_MODE_SEL_0_REG		0x0194
#define RTSDS_930X_SDS_MODE_SEL_1_REG		0x02a0
#define RTSDS_930X_SDS_MODE_SEL_2_REG		0x02a4
#define RTSDS_930X_SDS_MODE_SEL_3_REG		0x0198
#define RTSDS_930X_SDS_SUBMODE_CTRL0_REG	0x01cc
#define RTSDS_930X_SDS_SUBMODE_CTRL1_REG	0x02d8

#define RTSDS_930X_SDS_MODE_MASK		GENMASK(4, 0)
#define RTSDS_930X_SDS_SUBMODE_MASK		GENMASK(4, 0)

#define RTSDS_931X_SERDES_MODE_CTRL_REG(sid)	(0x13cc + (sid & ~3))
#define RTSDS_931X_PS_SDS_OFF_MODE_CTRL_REG	0x13f4

#define RTSDS_931X_CFG_FAMILY			0x9310
#define RTSDS_931X_CFG_SDS_CNT			14
#define RTSDS_931X_CFG_PAGE_CNT			576

#define RTSDS_931X_SDS_MODE_FORCE_SETUP		0x80
#define RTSDS_931X_SDS_MODE_READ_MASK		GENMASK(4, 0)
#define RTSDS_931X_SDS_MODE_BPOS(sid)		((sid & 3) << 3)
#define RTSDS_931X_SDS_MODE_WRITE_MASK		GENMASK(7, 0)
#define RTSDS_931X_SDS_SUBMODE_MASK		GENMASK(11, 6)

#define RTSDS_93XX_REG_MODEL_NAME_INFO		0x0004
#define RTSDS_93XX_SDS_CHIP_INFO_EN		0xa0000
#define RTSDS_93XX_SDS_CHIP_RL_ID_MASK		GENMASK(31, 28)

#define RTSDS_93XX_SDS_CMD_READ			0
#define RTSDS_93XX_SDS_CMD_WRITE		BIT(1)
#define RTSDS_93XX_SDS_CMD_BUSY			BIT(0)
#define RTSDS_93XX_SDS_CMD_SID_MASK		GENMASK(6, 2)
#define RTSDS_93XX_SDS_CMD_PAGE_MASK		GENMASK(12, 7)
#define RTSDS_93XX_SDS_CMD_REG_MASK		GENMASK(17, 13)

/*
 * All the following firmware constants are fixed and must not be changed. Otherwise existing
 * firmware files in the wild will break. If new events, operations or modes need to be added,
 * new numbers have to be assigned and reordering is strictly forbidden.
 */

#define RTSDS_FW_MAGIC				0x83009300

#define RTSDS_FW_EVT_UNDEFINED			0
#define RTSDS_FW_EVT_INIT			1
#define RTSDS_FW_EVT_POWER_ON			2
#define RTSDS_FW_EVT_POST_POWER_OFF		3
#define RTSDS_FW_EVT_POST_RESET			4
#define RTSDS_FW_EVT_POST_SET_MODE		5
#define RTSDS_FW_EVT_PRE_POWER_OFF		6
#define RTSDS_FW_EVT_PRE_RESET			7
#define RTSDS_FW_EVT_PRE_SET_MODE		8
#define RTSDS_FW_EVT_SETUP			9
#define RTSDS_FW_EVT_MAX			10 /* end marker */

#define RTSDS_FW_OP_UNDEFINED			0
#define RTSDS_FW_OP_MASK			1 /* modify register with <mask> and <value> */
#define RTSDS_FW_OP_WAIT			2 /* delay for <value> milliseconds */
#define RTSDS_FW_OP_MAX				3 /* end marker */

#define RTSDS_FW_MODE_UNDEFINED			0
#define RTSDS_FW_MODE_ALL			1
#define RTSDS_FW_MODE_HSGMII			2
#define RTSDS_FW_MODE_SGMII			3
#define RTSDS_FW_MODE_QSGMII			4
#define RTSDS_FW_MODE_QUSGMII			5
#define RTSDS_FW_MODE_USXGMII			6
#define RTSDS_FW_MODE_XGMII			7
#define RTSDS_FW_MODE_1000BASEX			8
#define RTSDS_FW_MODE_100BASEX			9
#define RTSDS_FW_MODE_10GBASER			10
#define RTSDS_FW_MODE_2500BASEX			11
#define RTSDS_FW_MODE_MAX			12 /* end marker */

struct __packed rtsds_fw_dir {
	uint32_t evtid;
	uint32_t offset;
	uint32_t len;
	uint32_t _future_use;
};

struct __packed rtsds_fw_head {
	u32 magic;
	u32 checksum;
	u32 filesize;
	u32 dirsize;
	struct rtsds_fw_dir dir[];
};

struct __packed rtsds_fw_seq {
	u16 mode;
	u16 ports;
	u16 action;
	u16 page;
	u16 reg;
	u16 mask;
	u16 val;
	u16 _future_use;
};

struct rtsds_sds {
	struct phy *phy;
	int mode;
	int speed;
	int link;
	int min_port;
	int max_port;
};

struct rtsds_soc {
	u32 model_id;
	u32 model_version;
	char model_name[16];
	u32 chip_id;
	u32 chip_version;
	char chip_name[16];
};

struct rtsds_ctrl {
	struct rtsds_soc soc;
	struct device *dev;
	struct regmap *regmap;
	u32 regbase;
	struct mutex lock;
	struct rtsds_cfg *cfg;
	struct rtsds_sds sds[RTSDS_931X_CFG_SDS_CNT];
	const struct firmware *firmware;
};

struct rtsds_macro {
	struct rtsds_ctrl *ctrl;
	u32 sid;
};

struct rtsds_cfg {
	u32 family;
	u32 sds_cnt;
	u32 page_cnt;
	void (*setup)(struct rtsds_ctrl *ctrl);
	int (*read)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg);
	int (*write_bits)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg, u32 mask, u32 val);
	int (*reset)(struct rtsds_ctrl *ctrl, u32 idx);
	int (*set_hwmode)(struct rtsds_ctrl *ctrl, u32 idx, int oldmode, int newmode);
	int (*get_hwmode)(struct rtsds_ctrl *ctrl, u32 idx);
	int mode_map[PHY_INTERFACE_MODE_MAX];
};

#endif /* _PHY_RTK_OTTO_SERDES_H */
