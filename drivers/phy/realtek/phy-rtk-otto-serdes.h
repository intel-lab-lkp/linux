/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek RTL838x, RTL839x, RTL930x & RTL931x SerDes PHY driver
 * Copyright (c) 2024 Markus Stockhausen <markus.stockhausen@gmx.de>
 */

#ifndef _PHY_RTK_OTTO_SERDES_H
#define _PHY_RTK_OTTO_SERDES_H

#define RTSDS_SWITCH_ADDR_BASE		(0xbb000000)
#define RTSDS_REG(x)			((void __iomem __force *)RTSDS_SWITCH_ADDR_BASE + (x))
#define iomask32(mask, value, addr)	iowrite32((ioread32(addr) & ~(mask)) | (value), addr)

#define RTSDS_PAGE_SDS				0x00
#define RTSDS_PAGE_SDS_EXT			0x01
#define RTSDS_PAGE_FIB				0x02
#define RTSDS_PAGE_FIB_EXT			0x03
#define RTSDS_PAGE_ANA_RG_EXT			0x09
#define RTSDS_PAGE_ANA_TG_EXT			0x0b

#define RTSDS_BIT_INV_HSO			BIT(8)
#define RTSDS_BIT_INV_HSI			BIT(9)
#define RTSDS_BIT_SOFT_RST			BIT(6)
#define RTSDS_BIT_SDS_EN_RX			BIT(1)
#define RTSDS_BIT_SDS_EN_TX			BIT(0)
#define RTSDS_BIT_RX_SELF			BIT(9)
#define RTSDS_BIT_RX_SELF_10G			BIT(15)

#define RTSDS_838X_FAMILY			0x8380
#define RTSDS_838X_SDS_CNT			6
#define RTSDS_838X_PAGE_CNT			4
#define RTSDS_838X_INT_MODE_CTRL		RTSDS_REG(0x005c)
#define RTSDS_838X_MODEL_NAME_INFO		RTSDS_REG(0x00d4)
#define RTSDS_838X_SDS_MODE_SEL			RTSDS_REG(0x0028)

#define RTSDS_839X_FAMILY			0x8390
#define RTSDS_839X_SDS_CNT			14
#define RTSDS_839X_PAGE_CNT			12
#define RTSDS_839X_MAC_SERDES_IF_CTRL		RTSDS_REG(0x0008)
#define RTSDS_839X_MODEL_NAME_INFO		RTSDS_REG(0x0ff0)

#define RTSDS_930X_FAMILY			0x9300
#define RTSDS_930X_SDS_CNT			12
#define RTSDS_930X_PAGE_CNT			64
#define RTSDS_930X_MODEL_NAME_INFO		RTSDS_REG(0x0004)
#define RTSDS_930X_SDS_MODE_SEL_0		RTSDS_REG(0x0194)
#define RTSDS_930X_SDS_MODE_SEL_1		RTSDS_REG(0x02a0)
#define RTSDS_930X_SDS_MODE_SEL_2		RTSDS_REG(0x02a4)
#define RTSDS_930X_SDS_MODE_SEL_3		RTSDS_REG(0x0198)
#define RTSDS_930X_SDS_SUBMODE_CTRL0		RTSDS_REG(0x01cc)
#define RTSDS_930X_SDS_SUBMODE_CTRL1		RTSDS_REG(0x02d8)

#define RTSDS_931X_FAMILY			0x9310
#define RTSDS_931X_SDS_CNT			14
#define RTSDS_931X_PAGE_CNT			576
#define RTSDS_931X_SERDES_MODE_CTRL		RTSDS_REG(0x13cc)
#define RTSDS_931X_PS_SERDES_OFF_MODE_CTRL	RTSDS_REG(0x13f4)
#define RTSDS_931X_SDS_FORCE_SETUP		0x80

#define RTSDS_93XX_SDS_READ			0
#define RTSDS_93XX_SDS_WRITE			BIT(1)
#define RTSDS_93XX_SDS_BUSY			BIT(0)
#define RTSDS_93XX_MODEL_NAME_INFO		RTSDS_REG(0x0004)

#define RTSDS_COMBOMODE(mode, submode)		(0x10000 | (mode << 8) | submode)
#define RTSDS_MODE(combomode)			((combomode >> 8) & 0xff)
#define RTSDS_SUBMODE(combomode)		(combomode & 0xff)

/*
 * All the following firmware constants are fixed and must not be changed. Otherwise existing
 * firmware files in the wild will break. If new events, operations or modes need to be added,
 * new id numbers have to be assigned and reordering is strictly forbidden.
 */

#define RTSDS_FW_MAGIC				0x83009300

#define RTSDS_FW_EVT_SETUP			0
#define RTSDS_FW_EVT_INIT			1
#define RTSDS_FW_EVT_POWER_ON			2
#define RTSDS_FW_EVT_PRE_SET_MODE		3
#define RTSDS_FW_EVT_POST_SET_MODE		4
#define RTSDS_FW_EVT_PRE_RESET			5
#define RTSDS_FW_EVT_POST_RESET			6
#define RTSDS_FW_EVT_PRE_POWER_OFF		7
#define RTSDS_FW_EVT_POST_POWER_OFF		8
#define RTSDS_FW_EVT_CNT			9

#define RTSDS_FW_OP_STOP			0
#define RTSDS_FW_OP_MASK			1
#define RTSDS_FW_OP_WAIT			2

#define RTSDS_FW_MODE_ALL			0
#define RTSDS_FW_MODE_QSGMII			1
#define RTSDS_FW_MODE_XGMII			2
#define RTSDS_FW_MODE_USXGMII			3
#define RTSDS_FW_MODE_1000BASEX			4
#define RTSDS_FW_MODE_2500BASEX			5
#define RTSDS_FW_MODE_10GBASER			6

struct __packed rtsds_fw_dir {
	uint32_t evtid;
	uint32_t offset;
};

struct __packed rtsds_fw_head {
	u32 magic;
	u32 checksum;
	u32 filesize;
	u32 dirsize;
	struct rtsds_fw_dir dir[];
};

struct __packed rtsds_fw_seq {
	u16 action;
	u16 mode;
	u16 ports;
	u16 page;
	u16 reg;
	u16 val;
	u16 mask;
	u16 align;
};

struct rtsds_sds {
	struct phy *phy;
	int mode;
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
	void __iomem *base;
	struct mutex lock;
	struct rtsds_conf *conf;
	struct rtsds_sds sds[RTSDS_931X_SDS_CNT];
	const struct firmware *firmware;
};

struct rtsds_macro {
	struct rtsds_ctrl *ctrl;
	u32 sid;
};

struct rtsds_conf {
	u32 family;
	u32 sds_cnt;
	u32 page_cnt;
	void (*setup)(struct rtsds_ctrl *ctrl);
	int (*read)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg);
	int (*mask)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg, u32 val, u32 mask);
	int (*reset)(struct rtsds_ctrl *ctrl, u32 idx);
	int (*set_hwmode)(struct rtsds_ctrl *ctrl, u32 idx, int mode);
	int (*get_hwmode)(struct rtsds_ctrl *ctrl, u32 idx);
	int mode_map[PHY_INTERFACE_MODE_MAX];
};

#endif /* _PHY_RTK_OTTO_SERDES_H */
