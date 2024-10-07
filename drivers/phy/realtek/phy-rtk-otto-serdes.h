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

#define RTSDS_BITS_INV_HSO			BIT(8)
#define RTSDS_BITS_INV_HSI			BIT(9)
#define RTSDS_BITS_SOFT_RST			BIT(6)
#define RTSDS_BITS_SDS_EN			GENMASK(1, 0)

#define RTSDS_EVENT_SETUP			0
#define RTSDS_EVENT_CNT				1

#define RTSDS_SEQ_STOP				0
#define RTSDS_SEQ_MASK				1
#define RTSDS_SEQ_WAIT				2

#define RTSDS_838X_SDS_CNT			6
#define RTSDS_838X_PAGE_CNT			4
#define RTSDS_838X_SDS_MODE_SEL			RTSDS_REG(0x0028)
#define RTSDS_838X_INT_MODE_CTRL		RTSDS_REG(0x005c)

#define RTSDS_839X_SDS_CNT			14
#define RTSDS_839X_PAGE_CNT			12
#define RTSDS_839X_MAC_SERDES_IF_CTRL		RTSDS_REG(0x0008)

#define RTSDS_930X_SDS_CNT			12
#define RTSDS_930X_PAGE_CNT			64
#define RTSDS_930X_SDS_MODE_SEL_0		RTSDS_REG(0x0194)
#define RTSDS_930X_SDS_MODE_SEL_1		RTSDS_REG(0x02a0)
#define RTSDS_930X_SDS_MODE_SEL_2		RTSDS_REG(0x02a4)
#define RTSDS_930X_SDS_MODE_SEL_3		RTSDS_REG(0x0198)
#define RTSDS_930X_SDS_SUBMODE_CTRL0		RTSDS_REG(0x01cc)
#define RTSDS_930X_SDS_SUBMODE_CTRL1		RTSDS_REG(0x02d8)

#define RTSDS_931X_SDS_CNT			14
#define RTSDS_931X_PAGE_CNT			192
#define RTSDS_931X_SERDES_MODE_CTRL		RTSDS_REG(0x13cc)
#define RTSDS_931X_PS_SERDES_OFF_MODE_CTRL	RTSDS_REG(0x13f4)
#define RTSDS_931X_SDS_FORCE_SETUP		0x80

#define RTSDS_93XX_SDS_READ			0x1
#define RTSDS_93XX_SDS_WRITE			0x3
#define RTSDS_93XX_SDS_BUSY			0x1

#define RTSDS_COMBOMODE(mode, submode)		(0x10000 | (mode << 8) | submode)
#define RTSDS_MODE(combomode)			((combomode >> 8) & 0xff)
#define RTSDS_SUBMODE(combomode)		(combomode & 0xff)

struct __packed rtsds_seq {
	u8 action;
	u8 mode;
	u16 ports;
	u16 page;
	u16 reg;
	u16 val;
	u16 mask;
};

struct rtsds_sds {
	struct phy *phy;
	int mode;
	int link;
	int min_port;
	int max_port;
};

struct rtsds_ctrl {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	u32 sds_mask;
	struct rtsds_conf *conf;
	struct rtsds_sds sds[RTSDS_931X_SDS_CNT];
	struct rtsds_seq *sequence[RTSDS_EVENT_CNT];
};

struct rtsds_macro {
	struct rtsds_ctrl *ctrl;
	u32 sid;
};

struct rtsds_conf {
	u32 sds_cnt;
	u32 page_cnt;
	int (*read)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg);
	int (*mask)(struct rtsds_ctrl *ctrl, u32 idx, u32 page, u32 reg, u32 val, u32 mask);
	int (*reset)(struct rtsds_ctrl *ctrl, u32 idx);
	int (*set_mode)(struct rtsds_ctrl *ctrl, u32 idx, int mode);
	int (*get_mode)(struct rtsds_ctrl *ctrl, u32 idx);
	int mode_map[PHY_INTERFACE_MODE_MAX];
	struct rtsds_seq *sequence[RTSDS_EVENT_CNT];
};

/*
 * This SerDes module should be written in quite a clean way so that direct calls are
 * not needed. The following functions are provided just in case ...
 */

int rtsds_read(struct phy *phy, u32 page, u32 reg);
int rtsds_write(struct phy *phy, u32 page, u32 reg, u32 val);
int rtsds_mask(struct phy *phy, u32 page, u32 reg, u32 val, u32 mask);

#endif /* _PHY_RTK_OTTO_SERDES_H */
