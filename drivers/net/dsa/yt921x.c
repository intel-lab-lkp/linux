// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Motorcomm YT921x Switch
 *
 * Should work on YT9213/YT9214/YT9215/YT9218, but only tested on YT9215+SGMII,
 * be sure to do your own checks before porting to another chip.
 *
 * Copyright (c) 2025 David Yang
 */

#include <linux/if_bridge.h>
#include <linux/if_hsr.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <net/dsa.h>

/******** hardware definitions ********/

#define YT921X_SMI_SWITCHID_M		GENMASK(3, 2)
#define  YT921X_SMI_SWITCHID(x)			FIELD_PREP(YT921X_SMI_SWITCHID_M, (x))
#define YT921X_SMI_AD			BIT(1)
#define  YT921X_SMI_ADDR			0
#define  YT921X_SMI_DATA			YT921X_SMI_AD
#define YT921X_SMI_RW			BIT(0)
#define  YT921X_SMI_WRITE			0
#define  YT921X_SMI_READ			YT921X_SMI_RW

#define YT921X_SWITCHID_NUM		4

#define YT921X_RST			0x80000
#define  YT921X_RST_HW				BIT(31)
#define  YT921X_RST_SW				BIT(1)
#define YT921X_FUNC			0x80004
#define  YT921X_FUNC_MIB			BIT(1)
#define YT921X_CHIP_ID			0x80008
#define  YT921X_CHIP_ID_MAJOR			GENMASK(31, 16)
#define YT921X_EXT_CPU_PORT		0x8000c
#define  YT921X_EXT_CPU_PORT_TAG_EN		BIT(15)
#define  YT921X_EXT_CPU_PORT_PORT_EN		BIT(14)
#define  YT921X_EXT_CPU_PORT_PORT_M		GENMASK(3, 0)
#define   YT921X_EXT_CPU_PORT_PORT(x)			FIELD_PREP(YT921X_EXT_CPU_PORT_PORT_M, (x))
#define YT921X_CPU_TAG_TPID		0x80010
#define  YT921X_CPU_TAG_TPID_TPID_M		GENMASK(15, 0)
#define   YT921X_CPU_TAG_TPID_TPID_DEFAULT		0x9988
#define YT921X_SGMII_CTRL		0x80028
#define  YT921X_SGMII_CTRL_EXTIFn_TEST(port)	BIT((port) - 3)
#define  YT921X_SGMII_CTRL_EXTIFn(port)		BIT((port) - 8)
#define YT921X_SGMIIn(port)		(0x8008c + 4 * ((port) - 8))
#define  YT921X_SGMII_MODE_M			GENMASK(9, 7)
#define   YT921X_SGMII_MODE(x)				FIELD_PREP(YT921X_SGMII_MODE_M, (x))
#define   YT921X_SGMII_MODE_SGMII_MAC			YT921X_SGMII_MODE(0)
#define   YT921X_SGMII_MODE_SGMII_PHY			YT921X_SGMII_MODE(1)
#define   YT921X_SGMII_MODE_1000BASEX			YT921X_SGMII_MODE(2)
#define   YT921X_SGMII_MODE_100BASEX			YT921X_SGMII_MODE(3)
#define   YT921X_SGMII_MODE_2500BASEX			YT921X_SGMII_MODE(4)
#define   YT921X_SGMII_MODE_BASEX			YT921X_SGMII_MODE(5)
#define   YT921X_SGMII_MODE_DISABLE			YT921X_SGMII_MODE(6)
#define  YT921X_SGMII_RX_PAUSE			BIT(6)
#define  YT921X_SGMII_TX_PAUSE			BIT(5)
#define  YT921X_SGMII_LINK			BIT(4)  /* force link */
#define  YT921X_SGMII_DUPLEX_FULL		BIT(3)
#define  YT921X_SGMII_SPEED_M			GENMASK(2, 0)
#define   YT921X_SGMII_SPEED(x)				FIELD_PREP(YT921X_SGMII_SPEED_M, (x))
#define   YT921X_SGMII_SPEED_10				YT921X_SGMII_SPEED(0)
#define   YT921X_SGMII_SPEED_100			YT921X_SGMII_SPEED(1)
#define   YT921X_SGMII_SPEED_1000			YT921X_SGMII_SPEED(2)
#define   YT921X_SGMII_SPEED_2500			YT921X_SGMII_SPEED(4)
#define YT921X_PORTn_CTRL(port)		(0x80100 + 4 * (port))
#define  YT921X_PORT_CTRL_PAUSE_AN		BIT(10)
#define YT921X_PORTn_STATUS(port)	(0x80200 + 4 * (port))
#define  YT921X_PORT_LINK			BIT(9)  /* CTRL: auto negotiation */
#define  YT921X_PORT_HALF_PAUSE			BIT(8)  /* Half-duplex back pressure mode */
#define  YT921X_PORT_DUPLEX_FULL		BIT(7)
#define  YT921X_PORT_RX_PAUSE			BIT(6)
#define  YT921X_PORT_TX_PAUSE			BIT(5)
#define  YT921X_PORT_RX_MAC_EN			BIT(4)
#define  YT921X_PORT_TX_MAC_EN			BIT(3)
#define  YT921X_PORT_SPEED_M			GENMASK(2, 0)
#define   YT921X_PORT_SPEED(x)				FIELD_PREP(YT921X_PORT_SPEED_M, (x))
#define   YT921X_PORT_SPEED_10				YT921X_PORT_SPEED(0)
#define   YT921X_PORT_SPEED_100				YT921X_PORT_SPEED(1)
#define   YT921X_PORT_SPEED_1000			YT921X_PORT_SPEED(2)
#define   YT921X_PORT_SPEED_2500			YT921X_PORT_SPEED(4)
#define YT921X_PON_STRAP_FUNC		0x80320
#define YT921X_PON_STRAP_VAL		0x80324
#define YT921X_PON_STRAP_CAP		0x80328
#define  YT921X_PON_STRAP_EEE			BIT(16)
#define  YT921X_PON_STRAP_LOOP_DETECT		BIT(7)
#define YT921X_MDIO_POLLINGn(port)	(0x80364 + 4 * ((port) - 8))
#define  YT921X_MDIO_POLLING_DUPLEX_FULL	BIT(4)
#define  YT921X_MDIO_POLLING_LINK		BIT(3)
#define  YT921X_MDIO_POLLING_SPEED_M		GENMASK(2, 0)
#define   YT921X_MDIO_POLLING_SPEED(x)			FIELD_PREP(YT921X_MDIO_POLLING_SPEED_M, (x))
#define   YT921X_MDIO_POLLING_SPEED_10			YT921X_MDIO_POLLING_SPEED(0)
#define   YT921X_MDIO_POLLING_SPEED_100			YT921X_MDIO_POLLING_SPEED(1)
#define   YT921X_MDIO_POLLING_SPEED_1000		YT921X_MDIO_POLLING_SPEED(2)
#define   YT921X_MDIO_POLLING_SPEED_2500		YT921X_MDIO_POLLING_SPEED(4)
#define YT921X_SENSOR			0x8036c
#define  YT921X_SENSOR_TEMP			BIT(18)
#define YT921X_TEMP			0x80374
#define YT921X_CHIP_MODE		0x80388
#define  YT921X_CHIP_MODE_MODE			GENMASK(1, 0)
#define YT921X_XMII_CTRL		0x80394
#define  YT921X_XMII_CTRL_EXTIFn(port)		BIT(9 - (port))  /* Yes, it's reversed */
#define YT921X_XMIIn(port)		(0x80400 + 8 * ((port) - 8))
#define  YT921X_XMII_MODE_M			GENMASK(31, 29)
#define   YT921X_XMII_MODE(x)				FIELD_PREP(YT921X_XMII_MODE_M, (x))
#define   YT921X_XMII_MODE_MII				YT921X_XMII_MODE(0)
#define   YT921X_XMII_MODE_REVMII			YT921X_XMII_MODE(1)
#define   YT921X_XMII_MODE_RMII				YT921X_XMII_MODE(2)
#define   YT921X_XMII_MODE_REVRMII			YT921X_XMII_MODE(3)
#define   YT921X_XMII_MODE_RGMII			YT921X_XMII_MODE(4)
#define   YT921X_XMII_MODE_DISABLE			YT921X_XMII_MODE(5)
#define  YT921X_XMII_LINK			BIT(19)  /* force link */
#define  YT921X_XMII_EN				BIT(18)
#define  YT921X_XMII_SOFT_RST			BIT(17)
#define  YT921X_XMII_RGMII_TX_DELAY_150PS_M	GENMASK(16, 13)
#define   YT921X_XMII_RGMII_TX_DELAY_150PS(x)		FIELD_PREP(YT921X_XMII_RGMII_TX_DELAY_150PS_M, (x))
#define  YT921X_XMII_TX_CLK_IN			BIT(11)
#define  YT921X_XMII_RX_CLK_IN			BIT(10)
#define  YT921X_XMII_RGMII_TX_DELAY_2NS		BIT(8)
#define  YT921X_XMII_RGMII_TX_CLK_OUT		BIT(7)
#define  YT921X_XMII_RGMII_RX_DELAY_150PS_M	GENMASK(6, 3)
#define   YT921X_XMII_RGMII_RX_DELAY_150PS(x)		FIELD_PREP(YT921X_XMII_RGMII_RX_DELAY_150PS_M, (x))
#define  YT921X_XMII_RMII_PHY_TX_CLK_OUT	BIT(2)
#define  YT921X_XMII_REVMII_TX_CLK_OUT		BIT(1)
#define  YT921X_XMII_REVMII_RX_CLK_OUT		BIT(0)

#define YT921X_MACn_FRAME(port)		(0x81008 + 0x1000 * (port))
#define  YT921X_MAC_FRAME_SIZE_M		GENMASK(21, 8)
#define   YT921X_MAC_FRAME_SIZE(x)			FIELD_PREP(YT921X_MAC_FRAME_SIZE_M, (x))

#define YT921X_EEEn_VAL(port)		(0xa0000 + 4 * (port))
#define  YT921X_EEE_VAL_DATA			BIT(1)

#define YT921X_EEE_CTRL			0xb0000
#define  YT921X_EEE_CTRL_ENn(port)		BIT(port)

#define YT921X_MIB_CTRL			0xc0004
#define  YT921X_MIB_CTRL_CLEAN			BIT(30)
#define  YT921X_MIB_CTRL_PORT_M			GENMASK(6, 3)
#define   YT921X_MIB_CTRL_PORT(x)			FIELD_PREP(YT921X_MIB_CTRL_PORT_M, (x))
#define  YT921X_MIB_CTRL_ONE_PORT		BIT(1)
#define  YT921X_MIB_CTRL_ALL_PORT		BIT(0)

#define YT921X_MIBn_DATA0(port)		(0xc0100 + 0x100 * (port))
#define YT921X_MIBn_DATAm(port, x)	(YT921X_MIBn_DATA0(port) + 4 * (x))

#define YT921X_EDATA_CTRL		0xe0000
#define  YT921X_EDATA_CTRL_ADDR_M		GENMASK(15, 8)
#define   YT921X_EDATA_CTRL_ADDR(x)			FIELD_PREP(YT921X_EDATA_CTRL_ADDR_M, (x))
#define  YT921X_EDATA_CTRL_OP_M			GENMASK(3, 0)
#define   YT921X_EDATA_CTRL_OP(x)			FIELD_PREP(YT921X_EDATA_CTRL_OP_M, (x))
#define   YT921X_EDATA_CTRL_READ			YT921X_EDATA_CTRL_OP(5)
#define YT921X_EDATA_DATA		0xe0004
#define  YT921X_EDATA_DATA_DATA_M			GENMASK(31, 24)
#define  YT921X_EDATA_DATA_STATUS_M		GENMASK(3, 0)
#define   YT921X_EDATA_DATA_STATUS(x)			FIELD_PREP(YT921X_EDATA_DATA_STATUS_M, (x))
#define   YT921X_EDATA_DATA_IDLE			YT921X_EDATA_DATA_STATUS(3)

#define YT921X_EXT_MBUS_OP		0x6a000
#define YT921X_INT_MBUS_OP		0xf0000
#define  YT921X_MBUS_OP_START			BIT(0)
#define YT921X_EXT_MBUS_CTRL		0x6a004
#define YT921X_INT_MBUS_CTRL		0xf0004
#define  YT921X_MBUS_CTRL_PORT_M		GENMASK(25, 21)
#define   YT921X_MBUS_CTRL_PORT(x)			FIELD_PREP(YT921X_MBUS_CTRL_PORT_M, (x))
#define  YT921X_MBUS_CTRL_REG_M			GENMASK(20, 16)
#define   YT921X_MBUS_CTRL_REG(x)			FIELD_PREP(YT921X_MBUS_CTRL_REG_M, (x))
#define  YT921X_MBUS_CTRL_TYPE_M		GENMASK(11, 8)  /* wild guess */
#define   YT921X_MBUS_CTRL_TYPE(x)			FIELD_PREP(YT921X_MBUS_CTRL_TYPE_M, (x))
#define   YT921X_MBUS_CTRL_TYPE_C22			YT921X_MBUS_CTRL_TYPE(4)
#define  YT921X_MBUS_CTRL_OP_M			GENMASK(3, 2)  /* wild guess */
#define   YT921X_MBUS_CTRL_OP(x)			FIELD_PREP(YT921X_MBUS_CTRL_OP_M, (x))
#define   YT921X_MBUS_CTRL_WRITE			YT921X_MBUS_CTRL_OP(1)
#define   YT921X_MBUS_CTRL_READ				YT921X_MBUS_CTRL_OP(2)
#define YT921X_EXT_MBUS_DOUT		0x6a008
#define YT921X_INT_MBUS_DOUT		0xf0008
#define YT921X_EXT_MBUS_DIN		0x6a00c
#define YT921X_INT_MBUS_DIN		0xf000c

#define YT921X_VLAN_IGR_FILTER		0x180280
#define  YT921X_VLAN_IGR_FILTER_PORTn_IGMP_BYPASS(port)	BIT((port) + 11)
#define  YT921X_VLAN_IGR_FILTER_PORTn_EN(port)	BIT(port)
#define YT921X_PORTn_ISOLATION(port)	(0x180294 + 4 * (port))
#define  YT921X_PORT_ISOLATION_BLOCKn(port)	BIT(port)
#define YT921X_PORTn_LEARN(port)	(0x1803d0 + 4 * (port))
#define  YT921X_PORT_LEARN_VID_LEARN_MULTI_EN	BIT(22)
#define  YT921X_PORT_LEARN_VID_LEARN_MODE	BIT(21)
#define  YT921X_PORT_LEARN_VID_LEARN_EN		BIT(20)
#define  YT921X_PORT_LEARN_SUSPEND_COPY_EN	BIT(19)
#define  YT921X_PORT_LEARN_SUSPEND_DROP_EN	BIT(18)
#define  YT921X_PORT_LEARN_DIS			BIT(17)
#define  YT921X_PORT_LEARN_LIMIT_EN		BIT(16)
#define  YT921X_PORT_LEARN_LIMIT_M		GENMASK(15, 8)
#define  YT921X_PORT_LEARN_LIMIT_EXCEED_DROP	BIT(2)
#define  YT921X_PORT_LEARN_MODE_M		GENMASK(1, 0)
#define   YT921X_PORT_LEARN_MODE(x)			FIELD_PREP(YT921X_PORT_LEARN_MODE_M, (x))
#define   YT921X_PORT_LEARN_MODE_AUTO			YT921X_PORT_LEARN_MODE(0)
#define   YT921X_PORT_LEARN_MODE_AUTO_AND_COPY		YT921X_PORT_LEARN_MODE(1)
#define   YT921X_PORT_LEARN_MODE_CPU_CONTROL		YT921X_PORT_LEARN_MODE(2)
#define YT921X_AGEING			0x180440
#define  YT921X_AGEING_INTERVAL_M		GENMASK(15, 0)
#define YT921X_FDB_IN0			0x180454
#define YT921X_FDB_IN1			0x180458
#define YT921X_FDB_IN2			0x18045c
#define YT921X_FDB_OP			0x180460
#define  YT921X_FDB_OP_INDEX_M			GENMASK(22, 11)
#define   YT921X_FDB_OP_INDEX(x)			FIELD_PREP(YT921X_FDB_OP_INDEX_M, (x))
#define  YT921X_FDB_OP_MODE_INDEX		BIT(10)  /* mac+fid / index */
#define  YT921X_FDB_OP_FLUSH_MCAST		BIT(9)  /* ucast / mcast */
#define  YT921X_FDB_OP_FLUSH_M			GENMASK(8, 7)
#define   YT921X_FDB_OP_FLUSH(x)			FIELD_PREP(YT921X_FDB_OP_FLUSH_M, (x))
#define   YT921X_FDB_OP_FLUSH_ALL			YT921X_FDB_OP_FLUSH(0)
#define   YT921X_FDB_OP_FLUSH_PORT			YT921X_FDB_OP_FLUSH(1)
#define   YT921X_FDB_OP_FLUSH_PORT_VID			YT921X_FDB_OP_FLUSH(2)
#define   YT921X_FDB_OP_FLUSH_VID			YT921X_FDB_OP_FLUSH(3)
#define  YT921X_FDB_OP_FLUSH_STATIC		BIT(6)
#define  YT921X_FDB_OP_NEXT_TYPE_M		GENMASK(5, 4)
#define   YT921X_FDB_OP_NEXT_TYPE(x)			FIELD_PREP(YT921X_FDB_OP_NEXT_TYPE_M, (x))
#define   YT921X_FDB_OP_NEXT_TYPE_UCAST_PORT		YT921X_FDB_OP_NEXT_TYPE(0)
#define   YT921X_FDB_OP_NEXT_TYPE_UCAST_VID		YT921X_FDB_OP_NEXT_TYPE(1)
#define   YT921X_FDB_OP_NEXT_TYPE_UCAST			YT921X_FDB_OP_NEXT_TYPE(2)
#define   YT921X_FDB_OP_NEXT_TYPE_MCAST			YT921X_FDB_OP_NEXT_TYPE(3)
#define  YT921X_FDB_OP_OP_M			GENMASK(3, 1)
#define   YT921X_FDB_OP_OP(x)				FIELD_PREP(YT921X_FDB_OP_OP_M, (x))
#define   YT921X_FDB_OP_OP_ADD				YT921X_FDB_OP_OP(0)
#define   YT921X_FDB_OP_OP_DEL				YT921X_FDB_OP_OP(1)
#define   YT921X_FDB_OP_OP_GET_ONE			YT921X_FDB_OP_OP(2)
#define   YT921X_FDB_OP_OP_GET_NEXT			YT921X_FDB_OP_OP(3)
#define   YT921X_FDB_OP_OP_FLUSH			YT921X_FDB_OP_OP(4)
#define  YT921X_FDB_OP_START			BIT(0)
#define YT921X_FDB_RESULT		0x180464
#define  YT921X_FDB_RESULT_DONE			BIT(15)
#define  YT921X_FDB_RESULT_NOTFOUND		BIT(14)
#define  YT921X_FDB_RESULT_OVERWRITED		BIT(13)
#define  YT921X_FDB_RESULT_INDEX_M		GENMASK(11, 0)
#define   YT921X_FDB_RESULT_INDEX(x)			FIELD_PREP(YT921X_FDB_RESULT_INDEX_M, (x))
#define YT921X_FDB_OUT0			0x1804b0
#define  YT921X_FDB_IO0_ADDR_HI4_M		GENMASK(31, 0)
#define YT921X_FDB_OUT1			0x1804b4
#define  YT921X_FDB_IO1_EGR_INT_PRI_EN		BIT(31)
#define  YT921X_FDB_IO1_STATUS_M		GENMASK(30, 28)
#define   YT921X_FDB_IO1_STATUS(x)			FIELD_PREP(YT921X_FDB_IO1_STATUS_M, (x))
#define   YT921X_FDB_IO1_STATUS_INVALID			YT921X_FDB_IO1_STATUS(0)
#define   YT921X_FDB_IO1_STATUS_MIN_TIME		YT921X_FDB_IO1_STATUS(1)
#define   YT921X_FDB_IO1_STATUS_MOVE_AGING_MAX_TIME	YT921X_FDB_IO1_STATUS(3)
#define   YT921X_FDB_IO1_STATUS_MAX_TIME		YT921X_FDB_IO1_STATUS(5)
#define   YT921X_FDB_IO1_STATUS_PENDING			YT921X_FDB_IO1_STATUS(6)
#define   YT921X_FDB_IO1_STATUS_STATIC			YT921X_FDB_IO1_STATUS(7)
#define  YT921X_FDB_IO1_FID_M			GENMASK(27, 16)  /* filtering ID (VID) */
#define   YT921X_FDB_IO1_FID(x)				FIELD_PREP(YT921X_FDB_IO1_FID_M, (x))
#define  YT921X_FDB_IO1_ADDR_LO2_M		GENMASK(15, 0)
#define YT921X_FDB_OUT2			0x1804b8
#define  YT921X_FDB_IO2_MOVE_AGING_STATUS_M	GENMASK(31, 30)
#define  YT921X_FDB_IO2_IGR_DROP		BIT(29)
#define  YT921X_FDB_IO2_EGR_PORTS_M		GENMASK(28, 18)
#define   YT921X_FDB_IO2_EGR_PORTS(x)			FIELD_PREP(YT921X_FDB_IO2_EGR_PORTS_M, (x))
#define  YT921X_FDB_IO2_EGR_DROP		BIT(17)
#define  YT921X_FDB_IO2_COPY_TO_CPU		BIT(16)
#define  YT921X_FDB_IO2_IGR_INT_PRI_EN		BIT(15)
#define  YT921X_FDB_IO2_INT_PRI_M		GENMASK(14, 12)
#define   YT921X_FDB_IO2_INT_PRI(x)			FIELD_PREP(YT921X_FDB_IO2_INT_PRI_M, (x))
#define  YT921X_FDB_IO2_NEW_VID_M		GENMASK(11, 0)
#define   YT921X_FDB_IO2_NEW_VID(x)			FIELD_PREP(YT921X_FDB_IO2_NEW_VID_M, (x))
#define YT921X_FILTER_UNK_UCAST		0x180508
#define YT921X_FILTER_UNK_MCAST		0x18050c
#define YT921X_FILTER_MCAST		0x180510
#define YT921X_FILTER_BCAST		0x180514
#define  YT921X_FILTER_PORTS_M			GENMASK(10, 0)
#define   YT921X_FILTER_PORTS(x)			FIELD_PREP(YT921X_FILTER_PORTS_M, (x))
#define  YT921X_FILTER_PORTn(port)		BIT(port)
#define YT921X_CPU_COPY			0x180690
#define  YT921X_CPU_COPY_FORCE_INT_PORT		BIT(2)
#define  YT921X_CPU_COPY_TO_INT_CPU		BIT(1)
#define  YT921X_CPU_COPY_TO_EXT_CPU		BIT(0)
#define YT921X_ACT_UNK_UCAST		0x180734
#define YT921X_ACT_UNK_MCAST		0x180738
#define  YT921X_ACT_UNK_MCAST_BYPASS_DROP_RMA	BIT(23)
#define  YT921X_ACT_UNK_MCAST_BYPASS_DROP_IGMP	BIT(22)
#define  YT921X_ACT_UNK_ACTn_M(port)		GENMASK(2 * (port) + 1, 2 * (port))
#define   YT921X_ACT_UNK_ACTn(port, x)			((x) << (2 * (port)))
#define   YT921X_ACT_UNK_ACTn_FORWARD(port)		YT921X_ACT_UNK_ACTn(port, 0)
#define   YT921X_ACT_UNK_ACTn_TRAP(port)		YT921X_ACT_UNK_ACTn(port, 1)  /* interrupt CPU */
#define   YT921X_ACT_UNK_ACTn_DROP(port)		YT921X_ACT_UNK_ACTn(port, 2)
#define   YT921X_ACT_UNK_ACTn_COPY(port)		YT921X_ACT_UNK_ACTn(port, 3)
#define YT921X_FDB_HW_FLUSH		0x180958
#define  YT921X_FDB_HW_FLUSH_ON_LINKDOWN	BIT(0)

#define YT921X_VLANn_CTRL(vlan)		(0x188000 + 8 * (vlan))
#define  YT921X_VLAN_CTRL_FID_LO9_M		GENMASK(31, 23)
#define  YT921X_VLAN_CTRL_LEARN_DIS		BIT(22)
#define  YT921X_VLAN_CTRL_INT_PRI_EN		BIT(21)
#define  YT921X_VLAN_CTRL_INT_PRI_M		GENMASK(20, 18)
#define  YT921X_VLAN_CTRL_PORTS_M		GENMASK(17, 7)
#define   YT921X_VLAN_CTRL_PORTS(x)			FIELD_PREP(YT921X_VLAN_CTRL_PORTS_M, (x))
#define  YT921X_VLAN_CTRL_PORTn(port)		BIT((port) + 7)
#define  YT921X_VLAN_CTRL_BYPASS_1X_AC		BIT(6)
#define  YT921X_VLAN_CTRL_METER_EN		BIT(5)
#define  YT921X_VLAN_CTRL_METER_ID_M		GENMASK(4, 0)
#define YT921X_VLANn_CTRL1(vlan)	(0x188004 + 8 * (vlan))
#define  YT921X_VLAN_CTRL1_UNTAG_PORTS_M	GENMASK(18, 8)
#define   YT921X_VLAN_CTRL1_UNTAG_PORTS(x)		FIELD_PREP(YT921X_VLAN_CTRL1_UNTAG_PORTS_M, (x))
#define  YT921X_VLAN_CTRL1_UNTAG_PORTn(port)	BIT((port) + 8)
#define  YT921X_VLAN_CTRL1_STP_ID_M		GENMASK(7, 4)
#define   YT921X_VLAN_CTRL1_STP_ID(x)			FIELD_PREP(YT921X_VLAN_CTRL1_STP_ID_M, (x))
#define  YT921X_VLAN_CTRL1_SVLAN_EN		BIT(3)
#define  YT921X_VLAN_CTRL1_FID_HI3_M		GENMASK(2, 0)

#define YT921X_PORTn_VLAN_CTRL(port)	(0x230010 + 4 * (port))
#define  YT921X_PORT_VLAN_CTRL_SVLAN_PRI_EN	BIT(31)
#define  YT921X_PORT_VLAN_CTRL_CVLAN_PRI_EN	BIT(30)
#define  YT921X_PORT_VLAN_CTRL_SVID_M		GENMASK(29, 18)
#define   YT921X_PORT_VLAN_CTRL_SVID(x)			FIELD_PREP(YT921X_PORT_VLAN_CTRL_SVID_M, (x))
#define  YT921X_PORT_VLAN_CTRL_CVID_M		GENMASK(17, 6)
#define   YT921X_PORT_VLAN_CTRL_CVID(x)			FIELD_PREP(YT921X_PORT_VLAN_CTRL_CVID_M, (x))
#define  YT921X_PORT_VLAN_CTRL_SVLAN_PRI_M	GENMASK(5, 3)
#define  YT921X_PORT_VLAN_CTRL_CVLAN_PRI_M	GENMASK(2, 0)
#define YT921X_PORTn_VLAN_CTRL1(port)	(0x230080 + 4 * (port))
#define  YT921X_PORT_VLAN_CTRL1_VLAN_RANGE_EN	BIT(8)
#define  YT921X_PORT_VLAN_CTRL1_VLAN_RANGE_PROFILE_ID_M	GENMASK(7, 4)
#define  YT921X_PORT_VLAN_CTRL1_SVLAN_DROP_TAGGED	BIT(3)
#define  YT921X_PORT_VLAN_CTRL1_SVLAN_DROP_UNTAGGED	BIT(2)
#define  YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_TAGGED	BIT(1)
#define  YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_UNTAGGED	BIT(0)

#define YT921X_MIRROR			0x300300
#define  YT921X_MIRROR_IGR_PORTS_M		GENMASK(26, 16)
#define   YT921X_MIRROR_IGR_PORTS(x)			FIELD_PREP(YT921X_MIRROR_IGR_PORTS_M, (x))
#define  YT921X_MIRROR_IGR_PORTn(port)		BIT((port) + 16)
#define  YT921X_MIRROR_EGR_PORTS_M		GENMASK(14, 4)
#define   YT921X_MIRROR_EGR_PORTS(x)			FIELD_PREP(YT921X_MIRROR_EGR_PORTS_M, (x))
#define  YT921X_MIRROR_EGR_PORTn(port)		BIT((port) + 4)
#define  YT921X_MIRROR_PORT_M			GENMASK(3, 0)
#define   YT921X_MIRROR_PORT(x)				FIELD_PREP(YT921X_MIRROR_PORT_M, (x))

#define YT921X_REG_END			0x400000  /* as long as reg space is below this */

#define YT921X_TAG_LEN			8

#define YT921X_EDATA_EXTMODE		0xfb
#define YT921X_EDATA_LEN		0x100

#define YT921X_FDB_NUM		4096

enum yt921x_fdb_entry_status {
	YT921X_FDB_ENTRY_STATUS_INVALID = 0,
	YT921X_FDB_ENTRY_STATUS_MIN_TIME = 1,
	YT921X_FDB_ENTRY_STATUS_MOVE_AGING_MAX_TIME = 3,
	YT921X_FDB_ENTRY_STATUS_MAX_TIME = 5,
	YT921X_FDB_ENTRY_STATUS_PENDING = 6,
	YT921X_FDB_ENTRY_STATUS_STATIC = 7,
};

#define YT921X_PVID_DEFAULT		1

#define YT921X_FRAME_SIZE_MAX		0x2400  /* 9216 */

#define YT921X_RST_DELAY_US		10000

struct yt921x_mib_desc {
	unsigned int size;
	unsigned int offset;
	const char *name;
};

#define MIB_DESC(_size, _offset, _name) {_size, _offset, _name}

static const struct yt921x_mib_desc yt921x_mib_descs[] = {
	MIB_DESC(1, 0x00, "RxBroadcast"),	/* rx broadcast pkts */
	MIB_DESC(1, 0x04, "RxPause"),		/* rx pause pkts */
	MIB_DESC(1, 0x08, "RxMulticast"),	/* rx multicast pkts, excluding pause and OAM */
	MIB_DESC(1, 0x0c, "RxCrcErr"),		/* rx crc err pkts, len >= 64B */

	MIB_DESC(1, 0x10, "RxAlignErr"),	/* rx pkts with odd number of bytes */
	MIB_DESC(1, 0x14, "RxUnderSizeErr"),	/* rx crc ok pkts, len < 64B */
	MIB_DESC(1, 0x18, "RxFragErr"),		/* rx crc err pkts, len < 64B */
	MIB_DESC(1, 0x1c, "RxPktSz64"),		/* rx pkts, len == 64B */

	MIB_DESC(1, 0x20, "RxPktSz65To127"),	/* rx pkts, len >= 65B and <= 127B */
	MIB_DESC(1, 0x24, "RxPktSz128To255"),	/* rx pkts, len >= 128B and <= 255B */
	MIB_DESC(1, 0x28, "RxPktSz256To511"),	/* rx pkts, len >= 256B and <= 511B */
	MIB_DESC(1, 0x2c, "RxPktSz512To1023"),	/* rx pkts, len >= 512B and <= 1023B */

	MIB_DESC(1, 0x30, "RxPktSz1024To1518"),	/* rx pkts, len >= 1024B and <= 1518B */
	MIB_DESC(1, 0x34, "RxPktSz1519ToMax"),	/* rx pkts, len >= 1519B */
	MIB_DESC(2, 0x38, "RxGoodBytes"),	/* total bytes of rx ok pkts */
	/* 0x3c */

	MIB_DESC(2, 0x40, "RxBadBytes"),	/* total bytes of rx err pkts */
	/* 0x44 */
	MIB_DESC(2, 0x48, "RxOverSzErr"),	/* rx pkts, len > mac frame size */
	/* 0x4c */

	MIB_DESC(1, 0x50, "RxDropped"),		/* rx dropped pkts, excluding crc err and pause */
	MIB_DESC(1, 0x54, "TxBroadcast"),	/* tx broadcast pkts */
	MIB_DESC(1, 0x58, "TxPause"),		/* tx pause pkts */
	MIB_DESC(1, 0x5c, "TxMulticast"),	/* tx multicast pkts, excluding pause and OAM */

	MIB_DESC(1, 0x60, "TxUnderSizeErr"),	/* tx pkts, len < 64B */
	MIB_DESC(1, 0x64, "TxPktSz64"),		/* tx pkts, len == 64B */
	MIB_DESC(1, 0x68, "TxPktSz65To127"),	/* tx pkts, len >= 65B and <= 127B */
	MIB_DESC(1, 0x6c, "TxPktSz128To255"),	/* tx pkts, len >= 128B and <= 255B */

	MIB_DESC(1, 0x70, "TxPktSz256To511"),	/* tx pkts, len >= 256B and <= 511B */
	MIB_DESC(1, 0x74, "TxPktSz512To1023"),	/* tx pkts, len >= 512B and <= 1023B */
	MIB_DESC(1, 0x78, "TxPktSz1024To1518"),	/* tx pkts, len >= 1024B and <= 1518B */
	MIB_DESC(1, 0x7c, "TxPktSz1519ToMax"),	/* tx pkts, len >= 1519B */

	MIB_DESC(2, 0x80, "TxGoodBytes"),	/* total bytes of tx ok pkts */
	/* 0x84 */
	MIB_DESC(2, 0x88, "TxCollision"),	/* collisions before 64B */
	/* 0x8c */

	MIB_DESC(1, 0x90, "TxExcessiveCollistion"),	/* aborted pkts due to too many colls */
	MIB_DESC(1, 0x94, "TxMultipleCollision"),	/* multiple collision for one mac */
	MIB_DESC(1, 0x98, "TxSingleCollision"),	/* one collision for one mac */
	MIB_DESC(1, 0x9c, "TxPkt"),		/* tx ok pkts */

	MIB_DESC(1, 0xa0, "TxDeferred"),	/* delayed pkts due to defer signal */
	MIB_DESC(1, 0xa4, "TxLateCollision"),	/* collisions after 64B */
	MIB_DESC(1, 0xa8, "RxOAM"),		/* rx OAM pkts */
	MIB_DESC(1, 0xac, "TxOAM"),		/* tx OAM pkts */
};

struct yt921x_mib_raw {
	u32 rx_broadcast;
	u32 rx_pause;
	u32 rx_multicast;
	u32 rx_crc_errors;

	u32 rx_frame_errors;
	u32 rx_undersize_errors;
	u32 rx_fragment_errors;
	u32 rx_64byte;

	u32 rx_65_127byte;
	u32 rx_128_255byte;
	u32 rx_256_511byte;
	u32 rx_512_1023byte;

	u32 rx_1024_1518byte;
	u32 rx_jumbo;
	u32 rx_good_bytes_hi;
	u32 rx_good_bytes_lo;

	u32 rx_bad_bytes_hi;
	u32 rx_bad_bytes_lo;
	u32 rx_over_errors_hi;
	u32 rx_over_errors_lo;

	u32 rx_dropped;
	u32 tx_broadcast;
	u32 tx_pause;
	u32 tx_multicast;

	u32 tx_undersize_errors;
	u32 tx_64byte;
	u32 tx_65_127byte;
	u32 tx_128_255byte;

	u32 tx_256_511byte;
	u32 tx_512_1023byte;
	u32 tx_1024_1518byte;
	u32 tx_jumbo;

	u32 tx_good_bytes_hi;
	u32 tx_good_bytes_lo;
	u32 tx_collisions_hi;
	u32 tx_collisions_lo;

	u32 tx_aborted_errors;
	u32 tx_multiple_collisions;
	u32 tx_single_collisions;
	u32 tx_good;

	u32 tx_defer;
	u32 tx_window_errors;
	u32 rx_oam;
	u32 tx_oam;
};

#define YT921X_MIBn_name(port, name) \
	(YT921X_MIBn_DATA0(port) + offsetof(struct yt921x_mib_raw, name))

#define yt921x_mib_raw_u64(mib, name) \
	(((u64)(mib)->name##_hi << 32) | (mib)->name##_lo)

struct yt921x_info {
	const char *name;
	u16 major;
	/* Unknown, seems to be plain enumeration */
	u8 mode;
	u8 extmode;
	u16 internal_mask;
	/* TODO: see comments in yt921x_dsa_phylink_get_caps() */
	u16 external_mask;
};

#define YT9215_MAJOR			0x9002
#define YT9218_MAJOR			0x9001

#define YT921X_PORT_MASK_INTn(port)	BIT(port)
#define YT921X_PORT_MASK_INT0_n(n)	GENMASK((n) - 1, 0)
#define YT921X_PORT_MASK_EXT0		BIT(8)
#define YT921X_PORT_MASK_EXT1		BIT(9)

#define yt921x_port_is_internal(port) ((port) < 8)
#define yt921x_port_is_external(port) (8 <= (port) && (port) < 9)

/* 8 internal + 2 external + 1 mcu */
#define YT921X_PORT_NUM			11

static const struct yt921x_info yt921x_infos[] = {
	{
		"YT9215SC", YT9215_MAJOR, 1, 0,
		YT921X_PORT_MASK_INT0_n(5),
		YT921X_PORT_MASK_EXT0 | YT921X_PORT_MASK_EXT1,
	},
	{
		"YT9215S", YT9215_MAJOR, 2, 0,
		YT921X_PORT_MASK_INT0_n(5),
		YT921X_PORT_MASK_EXT0 | YT921X_PORT_MASK_EXT1,
	},
	{
		"YT9215RB", YT9215_MAJOR, 3, 0,
		YT921X_PORT_MASK_INT0_n(5),
		YT921X_PORT_MASK_EXT0 | YT921X_PORT_MASK_EXT1,
	},
	{
		"YT9214NB", YT9215_MAJOR, 3, 2,
		YT921X_PORT_MASK_INTn(1) | YT921X_PORT_MASK_INTn(3),
		YT921X_PORT_MASK_EXT0 | YT921X_PORT_MASK_EXT1,
	},
	{
		"YT9213NB", YT9215_MAJOR, 3, 3,
		YT921X_PORT_MASK_INTn(1) | YT921X_PORT_MASK_INTn(3),
		YT921X_PORT_MASK_EXT1,
	},
	{
		"YT9218N", YT9218_MAJOR, 0, 0,
		YT921X_PORT_MASK_INT0_n(8),
		0,
	},
	{
		"YT9218MB", YT9218_MAJOR, 1, 0,
		YT921X_PORT_MASK_INT0_n(8),
		YT921X_PORT_MASK_EXT0 | YT921X_PORT_MASK_EXT1,
	},
	{}
};

#define yt921x_info_port_is_internal(info, port) \
	((info)->internal_mask & BIT(port))
#define yt921x_info_port_is_external(info, port) \
	((info)->external_mask & BIT(port))

/******** driver definitions ********/

#define YT921X_NAME	"yt921x"

#define YT921X_MDIO_SLEEP_US	10000
#define YT921X_MDIO_TIMEOUT_US	100000
#define YT921X_RST_TIMEOUT_US	100000

enum yt921x_fixed {
	YT921X_FIXED_NOTFIXED = 0,
	YT921X_FIXED_10,
	YT921X_FIXED_100,
	YT921X_FIXED_1000,
	YT921X_FIXED_2500,
};

struct yt921x_smi_ops {
	void (*acquire)(void *context);
	void (*release)(void *context);
	int (*read)(void *context, u32 reg, u32 *valp);
	int (*write)(void *context, u32 reg, u32 val);
};

struct yt921x_smi_mdio {
	struct mii_bus *bus;
	int addr;
	/* SWITCH_ID_1 / SWITCH_ID_0 of the device
	 *
	 * This is a way to multiplex multiple devices on the same MII phyaddr
	 * and should be configurable in DT. However, MDIO core simply doesn't
	 * allow multiple devices over one reg addr, so this is a fixed value
	 * for now until a solution is found.
	 *
	 * Keep this because we need switchid to form MII regaddrs anyway.
	 */
	unsigned char switchid;
};

/* TODO: SPI/I2C */

struct yt921x_port {
	struct yt921x_mib_raw mib;

	u32 tx_delay_150ps;
	u32 rx_delay_150ps;
	bool tx_delay_2ns;

	bool vlan_filtering;
	/* drop untagged frames if pvid is not set */
	u16 pvid;
	/* drop tagged frames if vid is not set */
	u16 vids_cnt;

	bool hairpin;
	bool isolated;
	u16 bridge_mask;
};

struct yt921x_priv {
	struct dsa_switch ds;

	const struct yt921x_info *info;
	u32 pon_strap_cap;
	u16 tag_eth_p;
	u16 cpu_ports_mask;

	/* slave bus */
	const struct yt921x_smi_ops *smi_ops;
	void *smi_ctx;

	/* mdio master bus */
	struct mii_bus *mbus_int;
	struct mii_bus *mbus_ext;

	struct yt921x_port ports[YT921X_PORT_NUM];

	u16 active_cpu_ports_mask;
	u16 eee_ports_mask;

	/* register prober */
	u32 reg_addr;
	u32 reg_val;
	bool reg_valid;
};

#define to_yt921x_priv(_ds) container_of_const(_ds, struct yt921x_priv, ds)
#define to_device(priv) ((priv)->ds.dev)

#define should_unreachable() \
	pr_err("%s: !!unreachable %d, please report a bug!\n", \
	       __func__, __LINE__)
#define consume_retval(res) do { \
	if (unlikely(res)) \
		pr_err("%s: %i\n", __func__, (res)); \
} while (0)

/******** smi ********/

static void yt921x_smi_acquire(struct yt921x_priv *priv)
{
	if (priv->smi_ops->acquire)
		priv->smi_ops->acquire(priv->smi_ctx);
}

static void yt921x_smi_release(struct yt921x_priv *priv)
{
	if (priv->smi_ops->release)
		priv->smi_ops->release(priv->smi_ctx);
}

static int yt921x_smi_read(struct yt921x_priv *priv, u32 reg, u32 *valp)
{
	return priv->smi_ops->read(priv->smi_ctx, reg, valp);
}

static int yt921x_smi_read_burst(struct yt921x_priv *priv, u32 reg, u32 *valp)
{
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_smi_read(priv, reg, valp);
	yt921x_smi_release(priv);

	return res;
}

static int yt921x_smi_write(struct yt921x_priv *priv, u32 reg, u32 val)
{
	return priv->smi_ops->write(priv->smi_ctx, reg, val);
}

static int yt921x_smi_write_burst(struct yt921x_priv *priv, u32 reg, u32 val)
{
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_smi_write(priv, reg, val);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_smi_update_bits(struct yt921x_priv *priv, u32 reg, u32 mask, u32 val)
{
	int res;
	u32 v;
	u32 u;

	res = yt921x_smi_read(priv, reg, &v);
	if (res)
		return res;

	u = v;
	u &= ~mask;
	u |= val;
	if (u == v)
		return 0;

	return yt921x_smi_write(priv, reg, u);
}

static int
yt921x_smi_update_bits_burst(struct yt921x_priv *priv, u32 reg, u32 mask,
			     u32 val)
{
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_smi_update_bits(priv, reg, mask, val);
	yt921x_smi_release(priv);

	return res;
}

static int yt921x_smi_set_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits(priv, reg, 0, mask);
}

static int
yt921x_smi_set_bits_burst(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits_burst(priv, reg, 0, mask);
}

static int yt921x_smi_clear_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits(priv, reg, mask, 0);
}

static int
yt921x_smi_clear_bits_burst(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits_burst(priv, reg, mask, 0);
}

static int
yt921x_smi_toggle_bits(struct yt921x_priv *priv, u32 reg, u32 mask, bool set)
{
	return yt921x_smi_update_bits(priv, reg, mask, !set ? 0 : mask);
}

/******** smi via mdio ********/

static void yt921x_smi_mdio_acquire(void *context)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
}

static void yt921x_smi_mdio_release(void *context)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;

	mutex_unlock(&bus->mdio_lock);
}

static void yt921x_smi_mdio_verify(u32 reg, u16 val)
{
	if (val == 0xfade)
		pr_warn("%s: Read 0xfade at 0x%x, which is a sign of "
			"non-existent register; consider reporting a bug if "
			"this happens again\n", __func__, reg);
	else if (val == 0xdead)
		pr_warn("%s: Read 0xdead at 0x%x, which is usually a serious "
			"data race condition; consider reporting a bug if "
			"this happens again\n", __func__, reg);
}

static int yt921x_smi_mdio_read(void *context, u32 reg, u32 *valp)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;
	int addr = mdio->addr;
	u32 reg_addr;
	u32 reg_data;
	u32 val;
	int res;

	reg_addr = YT921X_SMI_SWITCHID(mdio->switchid) | YT921X_SMI_ADDR |
		   YT921X_SMI_READ;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >> 16) & 0xffff);
	if (res)
		return res;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >>  0) & 0xffff);
	if (res)
		return res;

	reg_data = YT921X_SMI_SWITCHID(mdio->switchid) | YT921X_SMI_DATA |
		   YT921X_SMI_READ;
	res = __mdiobus_read(bus, addr, reg_data);
	if (res < 0)
		return res;
	yt921x_smi_mdio_verify(reg, res);
	val = res & 0xffff;
	res = __mdiobus_read(bus, addr, reg_data);
	if (res < 0)
		return res;
	yt921x_smi_mdio_verify(reg, res);
	val = (val << 16) | (res & 0xffff);

	*valp = val;
	return 0;
}

static int yt921x_smi_mdio_write(void *context, u32 reg, u32 val)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;
	int addr = mdio->addr;
	u32 reg_addr;
	u32 reg_data;
	int res;

	reg_addr = YT921X_SMI_SWITCHID(mdio->switchid) | YT921X_SMI_ADDR |
		   YT921X_SMI_WRITE;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >> 16) & 0xffff);
	if (res)
		return res;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >>  0) & 0xffff);
	if (res)
		return res;

	reg_data = YT921X_SMI_SWITCHID(mdio->switchid) | YT921X_SMI_DATA |
		   YT921X_SMI_WRITE;
	res = __mdiobus_write(bus, addr, reg_data, (val >> 16) & 0xffff);
	if (res)
		return res;
	res = __mdiobus_write(bus, addr, reg_data, (val >>  0) & 0xffff);
	if (res)
		return res;

	return 0;
}

static const struct yt921x_smi_ops yt921x_smi_ops_mdio = {
	.acquire = yt921x_smi_mdio_acquire,
	.release = yt921x_smi_mdio_release,
	.read = yt921x_smi_mdio_read,
	.write = yt921x_smi_mdio_write,
};

/* TODO: SPI/I2C */

/******** edata ********/

static int yt921x_edata_out(struct yt921x_priv *priv, u32 *valp)
{
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_EDATA_DATA, &val);
	if (res)
		return res;
	if ((val & YT921X_EDATA_DATA_STATUS_M) != YT921X_EDATA_DATA_IDLE) {
		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read_burst, res,
					(val & YT921X_EDATA_DATA_STATUS_M) ==
					YT921X_EDATA_DATA_IDLE,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_EDATA_DATA, &val);
		yt921x_smi_acquire(priv);
		if (res)
			return res;
	}

	*valp = val;
	return 0;
}

static int yt921x_edata_wait(struct yt921x_priv *priv)
{
	u32 val;

	return yt921x_edata_out(priv, &val);
}

static int
yt921x_edata_read_cont(struct yt921x_priv *priv, u8 addr, u8 *valp)
{
	u32 ctrl;
	u32 val;
	int res;

	ctrl = YT921X_EDATA_CTRL_ADDR(addr) | YT921X_EDATA_CTRL_READ;
	res = yt921x_smi_write(priv, YT921X_EDATA_CTRL, ctrl);
	if (res)
		return res;
	res = yt921x_edata_out(priv, &val);
	if (res)
		return res;

	*valp = FIELD_GET(YT921X_EDATA_DATA_DATA_M, val);
	return 0;
}

static int yt921x_edata_read(struct yt921x_priv *priv, u8 addr, u8 *valp)
{
	int res;

	res = yt921x_edata_wait(priv);
	if (res)
		return res;
	return yt921x_edata_read_cont(priv, addr, valp);
}

/******** internal interface mdio ********/

static int yt921x_intif_wait(struct yt921x_priv *priv)
{
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_INT_MBUS_OP, &val);
	if (res)
		return res;
	if ((val & YT921X_MBUS_OP_START) != 0) {
		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read_burst, res,
					(val & YT921X_MBUS_OP_START) == 0,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_INT_MBUS_OP, &val);
		yt921x_smi_acquire(priv);
		if (res)
			return res;
	}

	return 0;
}

static int
yt921x_intif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	struct device *dev = to_device(priv);
	u32 mask;
	u32 ctrl;
	u32 val;
	int res;

	res = yt921x_intif_wait(priv);
	if (res)
		return res;

	mask = YT921X_MBUS_CTRL_PORT_M | YT921X_MBUS_CTRL_REG_M |
	       YT921X_MBUS_CTRL_OP_M;
	ctrl = YT921X_MBUS_CTRL_PORT(port) | YT921X_MBUS_CTRL_REG(reg) |
	       YT921X_MBUS_CTRL_READ;
	res = yt921x_smi_update_bits(priv, YT921X_INT_MBUS_CTRL, mask, ctrl);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_INT_MBUS_OP, YT921X_MBUS_OP_START);
	if (res)
		return res;

	res = yt921x_intif_wait(priv);
	if (res)
		return res;
	res = yt921x_smi_read(priv, YT921X_INT_MBUS_DIN, &val);
	if (res)
		return res;

	if ((u16)val != val)
		dev_err(dev,
			"%s: port %d, reg 0x%x: Expected u16, got 0x%08x\n",
			__func__, port, reg, val);
	*valp = (u16)val;
	return 0;
}

static int
yt921x_intif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	u32 mask;
	u32 ctrl;
	int res;

	res = yt921x_intif_wait(priv);
	if (res)
		return res;

	mask = YT921X_MBUS_CTRL_PORT_M | YT921X_MBUS_CTRL_REG_M |
	       YT921X_MBUS_CTRL_OP_M;
	ctrl = YT921X_MBUS_CTRL_PORT(port) | YT921X_MBUS_CTRL_REG(reg) |
	       YT921X_MBUS_CTRL_WRITE;
	res = yt921x_smi_update_bits(priv, YT921X_INT_MBUS_CTRL, mask, ctrl);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_INT_MBUS_DOUT, val);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_INT_MBUS_OP, YT921X_MBUS_OP_START);
	if (res)
		return res;

	return yt921x_intif_wait(priv);
}

static int yt921x_mbus_int_read(struct mii_bus *mbus, int port, int reg)
{
	struct yt921x_priv *priv = mbus->priv;
	u16 val;
	int res;

	if (port >= YT921X_PORT_NUM)
		return 0xffff;

	yt921x_smi_acquire(priv);
	res = yt921x_intif_read(priv, port, reg, &val);
	yt921x_smi_release(priv);

	if (res)
		return res;
	return val;
}

static int
yt921x_mbus_int_write(struct mii_bus *mbus, int port, int reg, u16 data)
{
	struct yt921x_priv *priv = mbus->priv;
	int res;

	if (port >= YT921X_PORT_NUM)
		return 0;

	yt921x_smi_acquire(priv);
	res = yt921x_intif_write(priv, port, reg, data);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_mbus_int_init(struct yt921x_priv *priv, struct device_node *mnp)
{
	struct device *dev = to_device(priv);
	struct mii_bus *mbus;
	int res;

	mbus = devm_mdiobus_alloc(dev);
	if (!mbus)
		return -ENOMEM;

	mbus->name = "YT921x internal MDIO bus";
	snprintf(mbus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	mbus->priv = priv;
	mbus->read = yt921x_mbus_int_read;
	mbus->write = yt921x_mbus_int_write;
	mbus->parent = dev;
	mbus->phy_mask = (u32)~GENMASK(YT921X_PORT_NUM - 1, 0);

	if (!mnp)
		res = devm_mdiobus_register(dev, mbus);
	else
		res = devm_of_mdiobus_register(dev, mbus, mnp);
	if (res)
		return res;

	priv->mbus_int = mbus;

	return 0;
}

/******** external interface mdio ********/

static int yt921x_extif_wait(struct yt921x_priv *priv)
{
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_EXT_MBUS_OP, &val);
	if (res)
		return res;
	if ((val & YT921X_MBUS_OP_START) != 0) {
		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read_burst, res,
					(val & YT921X_MBUS_OP_START) == 0,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_EXT_MBUS_OP, &val);
		yt921x_smi_acquire(priv);
		if (res)
			return res;
	}

	return 0;
}

static int
yt921x_extif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	struct device *dev = to_device(priv);
	u32 mask;
	u32 ctrl;
	u32 val;
	int res;

	res = yt921x_extif_wait(priv);
	if (res)
		return res;

	mask = YT921X_MBUS_CTRL_PORT_M | YT921X_MBUS_CTRL_REG_M |
	       YT921X_MBUS_CTRL_TYPE_M | YT921X_MBUS_CTRL_OP_M;
	ctrl = YT921X_MBUS_CTRL_PORT(port) | YT921X_MBUS_CTRL_REG(reg) |
	       YT921X_MBUS_CTRL_TYPE_C22 | YT921X_MBUS_CTRL_READ;
	res = yt921x_smi_update_bits(priv, YT921X_EXT_MBUS_CTRL, mask, ctrl);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_EXT_MBUS_OP, YT921X_MBUS_OP_START);
	if (res)
		return res;

	res = yt921x_extif_wait(priv);
	if (res)
		return res;
	res = yt921x_smi_read(priv, YT921X_EXT_MBUS_DIN, &val);
	if (res)
		return res;

	if ((u16)val != val)
		dev_err(dev,
			"%s: port %d, reg 0x%x: Expected u16, got 0x%08x\n",
			__func__, port, reg, val);
	*valp = (u16)val;
	return 0;
}

static int
yt921x_extif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	u32 mask;
	u32 ctrl;
	int res;

	res = yt921x_extif_wait(priv);
	if (res)
		return res;

	mask = YT921X_MBUS_CTRL_PORT_M | YT921X_MBUS_CTRL_REG_M |
	       YT921X_MBUS_CTRL_TYPE_M | YT921X_MBUS_CTRL_OP_M;
	ctrl = YT921X_MBUS_CTRL_PORT(port) | YT921X_MBUS_CTRL_REG(reg) |
	       YT921X_MBUS_CTRL_TYPE_C22 | YT921X_MBUS_CTRL_WRITE;
	res = yt921x_smi_update_bits(priv, YT921X_EXT_MBUS_CTRL, mask, ctrl);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_EXT_MBUS_DOUT, val);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_EXT_MBUS_OP, YT921X_MBUS_OP_START);
	if (res)
		return res;

	return yt921x_extif_wait(priv);
}

static int yt921x_mbus_ext_read(struct mii_bus *mbus, int port, int reg)
{
	struct yt921x_priv *priv = mbus->priv;
	u16 val;
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_extif_read(priv, port, reg, &val);
	yt921x_smi_release(priv);

	if (res)
		return res;
	return val;
}

static int
yt921x_mbus_ext_write(struct mii_bus *mbus, int port, int reg, u16 data)
{
	struct yt921x_priv *priv = mbus->priv;
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_extif_write(priv, port, reg, data);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_mbus_ext_init(struct yt921x_priv *priv, struct device_node *mnp)
{
	struct device *dev = to_device(priv);
	struct mii_bus *mbus;
	int res;

	mbus = devm_mdiobus_alloc(dev);
	if (!mbus)
		return -ENOMEM;

	mbus->name = "YT921x external MDIO bus";
	snprintf(mbus->id, MII_BUS_ID_SIZE, "%s@ext", dev_name(dev));
	mbus->priv = priv;
	/* TODO: c45? */
	mbus->read = yt921x_mbus_ext_read;
	mbus->write = yt921x_mbus_ext_write;
	mbus->parent = dev;

	if (!mnp)
		res = devm_mdiobus_register(dev, mbus);
	else
		res = devm_of_mdiobus_register(dev, mbus, mnp);
	if (res)
		return res;

	priv->mbus_ext = mbus;

	return 0;
}

/******** mib ********/

static int yt921x_mib_read(struct yt921x_priv *priv, int port, void *data)
{
	unsigned char *buf = data;
	int res = 0;

	for (size_t i = 0; i < sizeof(struct yt921x_mib_raw);
	     i += sizeof(u32)) {
		res = yt921x_smi_read(priv, YT921X_MIBn_DATA0(port) + i,
				      (u32 *)&buf[i]);
		if (res)
			break;
	}
	return res;
}

static int yt921x_read_mib_burst(struct yt921x_priv *priv, int port)
{
	struct yt921x_mib_raw *mib = &priv->ports[port].mib;
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_mib_read(priv, port, mib);
	yt921x_smi_release(priv);

	return res;
}

static void
yt921x_dsa_get_strings(struct dsa_switch *ds, int port, u32 stringset,
		       uint8_t *data)
{
	if (stringset != ETH_SS_STATS)
		return;

	for (size_t i = 0; i < ARRAY_SIZE(yt921x_mib_descs); i++)
		ethtool_puts(&data, yt921x_mib_descs[i].name);
}

static void
yt921x_dsa_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct yt921x_mib_raw *mib = &priv->ports[port].mib;
	int res;

	res = yt921x_read_mib_burst(priv, port);
	consume_retval(res);

	for (size_t i = 0; i < ARRAY_SIZE(yt921x_mib_descs); i++) {
		const struct yt921x_mib_desc *desc = &yt921x_mib_descs[i];
		u32 *valp = (u32 *)((u8 *)mib + desc->offset);

		data[i] = valp[0];
		if (desc->size > 1) {
			data[i] <<= 32;
			data[i] |= valp[1];
		}
	}
}

static int yt921x_dsa_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(yt921x_mib_descs);
}

static void
yt921x_dsa_get_stats64(struct dsa_switch *ds, int port,
		       struct rtnl_link_stats64 *s)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct yt921x_mib_raw *mib = &priv->ports[port].mib;
	int res;

	res = yt921x_read_mib_burst(priv, port);
	consume_retval(res);

	s->rx_length_errors = (u64)mib->rx_undersize_errors +
			      mib->rx_fragment_errors;
	s->rx_over_errors = yt921x_mib_raw_u64(mib, rx_over_errors);
	s->rx_crc_errors = mib->rx_crc_errors;
	s->rx_frame_errors = mib->rx_frame_errors;
	/* s->rx_fifo_errors */
	/* s->rx_missed_errors */

	s->tx_aborted_errors = mib->tx_aborted_errors;
	/* s->tx_carrier_errors */
	s->tx_fifo_errors = mib->tx_undersize_errors;
	/* s->tx_heartbeat_errors */
	s->tx_window_errors = mib->tx_window_errors;

	s->rx_packets = (u64)mib->rx_64byte + mib->rx_65_127byte +
			mib->rx_128_255byte + mib->rx_256_511byte +
			mib->rx_512_1023byte + mib->rx_1024_1518byte +
			mib->rx_jumbo;
	s->tx_packets = (u64)mib->tx_64byte + mib->tx_65_127byte +
			mib->tx_128_255byte + mib->tx_256_511byte +
			mib->tx_512_1023byte + mib->tx_1024_1518byte +
			mib->tx_jumbo;
	s->rx_bytes = yt921x_mib_raw_u64(mib, rx_good_bytes) -
		      ETH_FCS_LEN * s->rx_packets;
	s->tx_bytes = yt921x_mib_raw_u64(mib, tx_good_bytes) -
		      ETH_FCS_LEN * s->tx_packets;
	s->rx_errors = (u64)mib->rx_crc_errors + s->rx_length_errors +
		       s->rx_over_errors;
	s->tx_errors = (u64)mib->tx_undersize_errors + mib->tx_aborted_errors +
		       mib->tx_window_errors;
	s->rx_dropped = mib->rx_dropped;
	/* s->tx_dropped */
	s->multicast = mib->rx_multicast;
	s->collisions = yt921x_mib_raw_u64(mib, tx_collisions);
}

static void
yt921x_dsa_get_pause_stats(struct dsa_switch *ds, int port,
			   struct ethtool_pause_stats *pause_stats)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	u32 tx_pause = 0;
	u32 rx_pause = 0;
	int res;

	yt921x_smi_acquire(priv);
	do {
		res = yt921x_smi_read(priv, YT921X_MIBn_name(port, tx_pause),
				      &tx_pause);
		if (res)
			break;
		res = yt921x_smi_read(priv, YT921X_MIBn_name(port, rx_pause),
				      &rx_pause);
	} while (0);
	yt921x_smi_release(priv);

	consume_retval(res);

	pause_stats->tx_pause_frames = tx_pause;
	pause_stats->rx_pause_frames = rx_pause;
}

/******** eee ********/

static int
yt921x_set_eee(struct yt921x_priv *priv, int port, struct ethtool_keee *e)
{
	struct device *dev = to_device(priv);
	bool enable = e->eee_enabled;
	u16 new_mask;
	int res;

	dev_dbg(dev, "%s: port %d, enable %d\n", __func__, port, enable);

	/* Enable / disable global EEE */
	new_mask = priv->eee_ports_mask;
	new_mask &= ~BIT(port);
	new_mask |= !enable ? 0 : BIT(port);

	if (!!new_mask != !!priv->eee_ports_mask) {
		dev_dbg(dev, "%s: toggle %d\n", __func__, !!new_mask);

		res = yt921x_smi_toggle_bits(priv, YT921X_PON_STRAP_FUNC,
					     YT921X_PON_STRAP_EEE, !!new_mask);
		if (res)
			return res;
		res = yt921x_smi_toggle_bits(priv, YT921X_PON_STRAP_VAL,
					     YT921X_PON_STRAP_EEE, !!new_mask);
		if (res)
			return res;
	}

	priv->eee_ports_mask = new_mask;

	/* Enable / disable port EEE */
	res = yt921x_smi_toggle_bits(priv, YT921X_EEE_CTRL,
				     YT921X_EEE_CTRL_ENn(port), enable);
	if (res)
		return res;
	res = yt921x_smi_toggle_bits(priv, YT921X_EEEn_VAL(port),
				     YT921X_EEE_VAL_DATA, enable);
	if (res)
		return res;

	return 0;
}

static bool yt921x_dsa_support_eee(struct dsa_switch *ds, int port)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);

	return (priv->pon_strap_cap & YT921X_PON_STRAP_EEE) != 0;
}

static int
yt921x_dsa_set_mac_eee(struct dsa_switch *ds, int port, struct ethtool_keee *e)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	int res;

	yt921x_smi_acquire(priv);
	res = yt921x_set_eee(priv, port, e);
	yt921x_smi_release(priv);

	return res;
}

/******** mtu ********/

static int
yt921x_dsa_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	/* Only serves as packet filter, since the frame size is always set to
	 * maximum after reset
	 */

	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct dsa_port *dp = dsa_to_port(ds, port);
	struct device *dev = to_device(priv);
	int frame_size;

	frame_size = new_mtu + ETH_HLEN + ETH_FCS_LEN;
	if (dsa_port_is_cpu(dp))
		frame_size += YT921X_TAG_LEN;

	dev_dbg(dev, "%s: port %d, mtu %d, frame size %d\n", __func__,
		port, new_mtu, frame_size);

	return yt921x_smi_update_bits_burst(priv, YT921X_MACn_FRAME(port),
					    YT921X_MAC_FRAME_SIZE_M,
					    YT921X_MAC_FRAME_SIZE(frame_size));
}

static int yt921x_dsa_port_max_mtu(struct dsa_switch *ds, int port)
{
	/* Don't want to brother dsa_port_is_cpu() here, so use a fixed value */
	return YT921X_FRAME_SIZE_MAX - ETH_HLEN - ETH_FCS_LEN - YT921X_TAG_LEN;
}

/******** mirror ********/

static void
yt921x_dsa_port_mirror_del(struct dsa_switch *ds, int port,
			   struct dsa_mall_mirror_tc_entry *mirror)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	u32 mask;
	int res;

	dev_dbg(dev, "%s: port %d, ingress %d\n", __func__, port,
		mirror->ingress);

	yt921x_smi_acquire(priv);
	if (mirror->ingress)
		mask = YT921X_MIRROR_IGR_PORTn(port);
	else
		mask = YT921X_MIRROR_EGR_PORTn(port);
	res = yt921x_smi_clear_bits(priv, YT921X_MIRROR, mask);
	yt921x_smi_release(priv);

	consume_retval(res);
}

static int
yt921x_dsa_port_mirror_add(struct dsa_switch *ds, int port,
			   struct dsa_mall_mirror_tc_entry *mirror,
			   bool ingress, struct netlink_ext_ack *extack)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	u32 ctrl;
	u32 val;
	int res;

	dev_dbg(dev, "%s: port %d, ingress %d\n", __func__, port, ingress);

	yt921x_smi_acquire(priv);
	do {
		u32 srcs;

		res = yt921x_smi_read(priv, YT921X_MIRROR, &val);
		if (res)
			break;

		if (ingress)
			srcs = YT921X_MIRROR_IGR_PORTn(port);
		else
			srcs = YT921X_MIRROR_EGR_PORTn(port);

		ctrl = val;
		ctrl |= srcs;
		ctrl &= ~YT921X_MIRROR_PORT_M;
		ctrl |= YT921X_MIRROR_PORT(mirror->to_local_port);
		if (ctrl == val)
			break;

		/* other mirror tasks & different dst port -> conflict */
		if ((val & ~srcs & (YT921X_MIRROR_EGR_PORTS_M |
				    YT921X_MIRROR_IGR_PORTS_M)) != 0 &&
		    ((ctrl ^ val) & YT921X_MIRROR_PORT_M) != 0) {
			res = -EEXIST;
			break;
		}

		res = yt921x_smi_write(priv, YT921X_MIRROR, ctrl);
	} while (0);
	yt921x_smi_release(priv);

	return res;
}

/******** vlan ********/

static int
yt921x_vlan_filtering(struct yt921x_priv *priv, int port, bool vlan_filtering)
{
	struct yt921x_port *pp = &priv->ports[port];
	u32 mask;
	u32 ctrl;
	int res;

	res = yt921x_smi_toggle_bits(priv, YT921X_VLAN_IGR_FILTER,
				     YT921X_VLAN_IGR_FILTER_PORTn_EN(port),
				     vlan_filtering);
	if (res)
		return res;

	mask = YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_TAGGED |
	       YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_UNTAGGED;
	ctrl = 0;
	if (vlan_filtering) {
		if (!pp->pvid)
			ctrl |= YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_UNTAGGED;
		if (!pp->vids_cnt)
			ctrl |= YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_TAGGED;
	}
	res = yt921x_smi_update_bits(priv, YT921X_PORTn_VLAN_CTRL1(port),
				     mask, ctrl);
	if (res)
		return res;

	pp->vlan_filtering = vlan_filtering;
	return 0;
}

static int
yt921x_vid_del(struct yt921x_priv *priv, int port, u16 vid)
{
	struct yt921x_port *pp = &priv->ports[port];
	u32 mask;
	u32 ctrl;
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_VLANn_CTRL(port), &val);
	if (res)
		return res;
	ctrl = val & ~YT921X_VLAN_CTRL_PORTn(port);
	if (ctrl == val)
		return 0;
	res = yt921x_smi_write(priv, YT921X_VLANn_CTRL(port), ctrl);
	if (res)
		return res;

	mask = YT921X_VLAN_CTRL1_UNTAG_PORTn(port);
	res = yt921x_smi_clear_bits(priv, YT921X_VLANn_CTRL1(port), mask);
	if (res)
		return res;

	if (pp->vlan_filtering && pp->vids_cnt <= 1) {
		mask = YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_TAGGED;
		res = yt921x_smi_set_bits(priv, YT921X_PORTn_VLAN_CTRL1(port),
					  mask);
		if (res)
			return res;
	}

	if (pp->vids_cnt <= 0)
		should_unreachable();
	else
		pp->vids_cnt--;
	return 0;
}

/* Seems port_vlan_add() is not state transition method... */
static int
yt921x_vid_set(struct yt921x_priv *priv, int port, u16 vid, bool untagged)
{
	struct yt921x_port *pp = &priv->ports[port];
	bool already_added;
	u32 mask;
	u32 ctrl;
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_VLANn_CTRL(port), &val);
	if (res)
		return res;
	ctrl = val | YT921X_VLAN_CTRL_PORTn(port);
	if (ctrl == val) {
		already_added = true;
	} else {
		already_added = false;
		res = yt921x_smi_write(priv, YT921X_VLANn_CTRL(port), ctrl);
		if (res)
			return res;
	}

	mask = YT921X_VLAN_CTRL1_UNTAG_PORTn(port);
	res = yt921x_smi_toggle_bits(priv, YT921X_VLANn_CTRL1(port), mask,
				     untagged);
	if (res)
		return res;

	mask = YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_TAGGED;
	res = yt921x_smi_clear_bits(priv, YT921X_PORTn_VLAN_CTRL1(port), mask);
	if (res)
		return res;

	if (!already_added) {
		if (pp->vids_cnt >= VLAN_N_VID - 1)
			should_unreachable();
		else
			pp->vids_cnt++;
	}
	return 0;
}

static int
yt921x_pvid_clear(struct yt921x_priv *priv, int port)
{
	struct yt921x_port *pp = &priv->ports[port];
	u32 mask;
	u32 ctrl;
	int res;

	mask = YT921X_PORT_VLAN_CTRL_CVID_M;
	ctrl = YT921X_PORT_VLAN_CTRL_CVID(YT921X_PVID_DEFAULT);
	res = yt921x_smi_update_bits(priv, YT921X_PORTn_VLAN_CTRL(port),
				     mask, ctrl);
	if (res)
		return res;

	if (pp->vlan_filtering) {
		mask = YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_UNTAGGED;
		res = yt921x_smi_set_bits(priv, YT921X_PORTn_VLAN_CTRL1(port),
					  mask);
		if (res)
			return res;
	}

	pp->pvid = 0;
	return 0;
}

static int
yt921x_pvid_set(struct yt921x_priv *priv, int port, u16 vid)
{
	struct yt921x_port *pp = &priv->ports[port];
	u32 mask;
	u32 ctrl;
	int res;

	mask = YT921X_PORT_VLAN_CTRL_CVID_M;
	ctrl = YT921X_PORT_VLAN_CTRL_CVID(vid);
	res = yt921x_smi_update_bits(priv, YT921X_PORTn_VLAN_CTRL(port),
				     mask, ctrl);
	if (res)
		return res;

	mask = YT921X_PORT_VLAN_CTRL1_CVLAN_DROP_UNTAGGED;
	res = yt921x_smi_clear_bits(priv, YT921X_PORTn_VLAN_CTRL1(port), mask);
	if (res)
		return res;

	pp->pvid = vid;
	return 0;
}

static int
yt921x_dsa_port_vlan_filtering(struct dsa_switch *ds, int port,
			       bool vlan_filtering,
			       struct netlink_ext_ack *extack)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d, enable %d\n", __func__, port,
		vlan_filtering);

	yt921x_smi_acquire(priv);
	res = yt921x_vlan_filtering(priv, port, vlan_filtering);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_vlan_del(struct dsa_switch *ds, int port,
			 const struct switchdev_obj_port_vlan *vlan)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct yt921x_port *pp = &priv->ports[port];
	struct device *dev = to_device(priv);
	u16 vid = vlan->vid;
	int res;

	dev_dbg(dev, "%s: port %d, vid %d\n", __func__, port, vid);

	yt921x_smi_acquire(priv);
	do {
		res = yt921x_vid_del(priv, port, vid);
		if (res)
			break;

		if (pp->pvid == vid) {
			res = yt921x_pvid_clear(priv, port);
			if (res)
				break;
		}
	} while (0);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_vlan_add(struct dsa_switch *ds, int port,
			 const struct switchdev_obj_port_vlan *vlan,
			 struct netlink_ext_ack *extack)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct yt921x_port *pp = &priv->ports[port];
	struct device *dev = to_device(priv);
	u16 vid = vlan->vid;
	int res;

	dev_dbg(dev, "%s: port %d, vid %d, flags 0x%x\n", __func__, port, vid,
		vlan->flags);

	yt921x_smi_acquire(priv);
	do {
		res = yt921x_vid_set(priv, port, vid,
				     (vlan->flags &
				      BRIDGE_VLAN_INFO_UNTAGGED) != 0);
		if (res)
			break;

		if ((vlan->flags & BRIDGE_VLAN_INFO_PVID) != 0) {
			res = yt921x_pvid_set(priv, port, vid);
			if (res)
				break;
		} else if (pp->pvid == vid) {
			res = yt921x_pvid_clear(priv, port);
			if (res)
				break;
		}
	} while (0);
	yt921x_smi_release(priv);

	return res;
}

/******** bridge ********/

/* It's caller's responsibility to decide whether ports_mask contains cpu
 * ports
 */
static int yt921x_bridge(struct yt921x_priv *priv, u16 ports_mask)
{
	u16 targets_mask = ports_mask & ~priv->cpu_ports_mask;
	u32 isolated_mask;
	u32 ctrl;
	int res;

	isolated_mask = 0;
	for (u16 pm = targets_mask; pm ; ) {
		struct yt921x_port *pp;
		int port = __fls(pm);

		pm &= ~BIT(port);
		pp = &priv->ports[port];

		if (pp->isolated)
			isolated_mask |= BIT(port);
	}

	/* Block from non-cpu bridge ports ... */
	for (u16 pm = targets_mask; pm ; ) {
		struct yt921x_port *pp;
		int port = __fls(pm);

		pm &= ~BIT(port);
		pp = &priv->ports[port];

		/* to non-bridge ports */
		ctrl = ~ports_mask;
		/* to isolated ports when isolated */
		if (pp->isolated)
			ctrl |= isolated_mask;
		/* to itself when non-hairpin */
		if (!pp->hairpin)
			ctrl |= BIT(port);
		else
			ctrl &= ~BIT(port);

		res = yt921x_smi_write(priv, YT921X_PORTn_ISOLATION(port),
				       ctrl);
		if (res)
			return res;
	}

	for (u16 pm = targets_mask; pm ; ) {
		struct yt921x_port *pp;
		int port = __fls(pm);

		pm &= ~BIT(port);
		pp = &priv->ports[port];

		pp->bridge_mask = ports_mask;
	}

	return 0;
}

static int yt921x_bridge_force(struct yt921x_priv *priv, u16 ports_mask)
{
	u16 targets_mask = ~ports_mask & (BIT(YT921X_PORT_NUM) - 1) &
			   ~priv->cpu_ports_mask;
	u32 mask;
	int res;

	res = yt921x_bridge(priv, ports_mask);
	if (res)
		return res;

	/* Block ... to non-cpu bridge ports */
	mask = ports_mask & ~priv->cpu_ports_mask;
	/* from non-cpu non-bridge ports */
	for (u16 pm = targets_mask; pm ; ) {
		int port = __fls(pm);

		pm &= ~BIT(port);
		res = yt921x_smi_set_bits(priv, YT921X_PORTn_ISOLATION(port),
					  mask);
		if (res)
			return res;
	}

	for (u16 pm = targets_mask; pm ; ) {
		struct yt921x_port *pp;
		int port = __fls(pm);

		pm &= ~BIT(port);
		pp = &priv->ports[port];

		pp->bridge_mask &= ~mask;
	}

	return 0;
}

static int
yt921x_bridge_flags(struct yt921x_priv *priv, int port,
		    struct switchdev_brport_flags flags)
{
	struct yt921x_port *pp = &priv->ports[port];
	bool do_flush;
	u32 mask;
	u32 ctrl;
	int res;

	if ((flags.mask & BR_LEARNING) != 0) {
		mask = YT921X_PORT_LEARN_DIS;
		ctrl = (flags.val & BR_LEARNING) == 0 ? YT921X_PORT_LEARN_DIS :
		       0;
		res = yt921x_smi_update_bits(priv, YT921X_PORTn_LEARN(port),
					     mask, ctrl);
		if (res)
			return res;
	}

	if ((flags.mask & BR_FLOOD) != 0) {
		mask = YT921X_ACT_UNK_ACTn_M(port);
		ctrl = (flags.val & BR_FLOOD) == 0 ?
		       YT921X_ACT_UNK_ACTn_DROP(port) :
		       YT921X_ACT_UNK_ACTn_FORWARD(port);
		res = yt921x_smi_update_bits(priv, YT921X_ACT_UNK_UCAST,
					     mask, ctrl);
		if (res)
			return res;
	}

	if ((flags.mask & BR_MCAST_FLOOD) != 0) {
		mask = YT921X_FILTER_PORTn(port);
		res = yt921x_smi_toggle_bits(priv, YT921X_FILTER_MCAST,
					     mask,
					     (flags.val & BR_MCAST_FLOOD) != 0);
		if (res)
			return res;
	}

	if ((flags.mask & BR_BCAST_FLOOD) != 0) {
		mask = YT921X_FILTER_PORTn(port);
		res = yt921x_smi_toggle_bits(priv, YT921X_FILTER_BCAST,
					     mask,
					     (flags.val & BR_BCAST_FLOOD) != 0);
		if (res)
			return res;
	}

	do_flush = false;
	if ((flags.mask & BR_HAIRPIN_MODE) != 0) {
		pp->hairpin = (flags.val & BR_HAIRPIN_MODE) != 0;
		do_flush = true;
	}
	if ((flags.mask & BR_ISOLATED) != 0) {
		pp->isolated = (flags.val & BR_ISOLATED) != 0;
		do_flush = true;
	}
	if (do_flush) {
		res = yt921x_bridge(priv, pp->bridge_mask);
		if (res)
			return res;
	}

	return 0;
}

static int
yt921x_dsa_port_pre_bridge_flags(struct dsa_switch *ds, int port,
				 struct switchdev_brport_flags flags,
				 struct netlink_ext_ack *extack)
{
	if ((flags.mask & ~(BR_HAIRPIN_MODE | BR_LEARNING | BR_FLOOD |
			    BR_MCAST_FLOOD | BR_BCAST_FLOOD |
			    BR_ISOLATED)) != 0)
		return -EINVAL;
	return 0;
}

static int
yt921x_dsa_port_bridge_flags(struct dsa_switch *ds, int port,
			     struct switchdev_brport_flags flags,
			     struct netlink_ext_ack *extack)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d, mask 0x%lx, flags 0x%lx\n", __func__, port,
		flags.mask, flags.val);

	yt921x_smi_acquire(priv);
	res = yt921x_bridge_flags(priv, port, flags);
	yt921x_smi_release(priv);

	return res;
}

static u32
dsa_bridge_ports(struct dsa_switch *ds, const struct net_device *bridge_dev)
{
	struct dsa_port *dp;
	u32 mask = 0;

	dsa_switch_for_each_user_port(dp, ds)
		if (dsa_port_offloads_bridge_dev(dp, bridge_dev))
			mask |= BIT(dp->index);

	return mask;
}

static void
yt921x_dsa_port_bridge_leave(struct dsa_switch *ds, int port,
			     struct dsa_bridge bridge)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	u16 ports_mask;
	int res;

	ports_mask = dsa_bridge_ports(ds, bridge.dev);

	dev_dbg(dev, "%s: port %d, mask 0x%x\n", __func__, port, ports_mask);

	ports_mask |= priv->cpu_ports_mask;

	yt921x_smi_acquire(priv);
	res = yt921x_bridge_force(priv, ports_mask);
	yt921x_smi_release(priv);

	consume_retval(res);
}

static int
yt921x_dsa_port_bridge_join(struct dsa_switch *ds, int port,
			    struct dsa_bridge bridge, bool *tx_fwd_offload,
			    struct netlink_ext_ack *extack)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	u16 ports_mask;
	int res;

	ports_mask = dsa_bridge_ports(ds, bridge.dev);

	dev_dbg(dev, "%s: port %d, mask 0x%x\n", __func__, port, ports_mask);

	ports_mask |= priv->cpu_ports_mask;

	yt921x_smi_acquire(priv);
	res = yt921x_bridge(priv, ports_mask);
	yt921x_smi_release(priv);

	return res;
}

/******** fdb ********/

static int yt921x_fdb_wait(struct yt921x_priv *priv, u32 *valp)
{
	struct device *dev = to_device(priv);
	u32 val;
	int res;

	res = yt921x_smi_read(priv, YT921X_FDB_RESULT, &val);
	if (res)
		return res;
	if ((val & YT921X_FDB_RESULT_DONE) == 0) {
		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read_burst, res,
					(val & YT921X_FDB_RESULT_DONE) != 0,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_FDB_RESULT,
					&val);
		yt921x_smi_acquire(priv);
		if (res) {
			dev_warn(dev, "Probably an FDB hang happened\n");
			return res;
		}
	}

	*valp = val;
	return 0;
}

static int
yt921x_fdb_in01(struct yt921x_priv *priv, const unsigned char *addr,
		u16 vid, u32 ctrl1)
{
	u32 ctrl;
	int res;

	ctrl = (addr[0] << 24) | (addr[1] << 16) | (addr[2] << 8) | addr[3];
	res = yt921x_smi_write(priv, YT921X_FDB_IN0, ctrl);
	if (res)
		return res;

	ctrl = ctrl1 | YT921X_FDB_IO1_FID(vid) | (addr[4] << 8) | addr[5];
	return yt921x_smi_write(priv, YT921X_FDB_IN1, ctrl);
}

static int
yt921x_fdb_has(struct yt921x_priv *priv, const unsigned char *addr, u16 vid,
	       u16 *indexp)
{
	u32 ctrl;
	u32 val;
	int res;

	res = yt921x_fdb_in01(priv, addr, vid, 0);
	if (res)
		return res;

	ctrl = 0;
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, ctrl);
	if (res)
		return res;

	ctrl = YT921X_FDB_OP_OP_GET_ONE | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;
	if ((val & YT921X_FDB_RESULT_NOTFOUND) == 0) {
		*indexp = YT921X_FDB_NUM;
		return 0;
	}

	*indexp = FIELD_GET(YT921X_FDB_RESULT_INDEX_M, val);
	return 0;
}

static int
yt921x_fdb_read(struct yt921x_priv *priv, unsigned char *addr, u16 *vidp,
		u16 *ports_maskp, u16 *indexp, u8 *statusp)
{
	struct device *dev = to_device(priv);
	u16 index;
	u32 data0;
	u32 data1;
	u32 data2;
	u32 val;
	int res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;
	if ((val & YT921X_FDB_RESULT_NOTFOUND) != 0) {
		*ports_maskp = 0;
		return 0;
	}
	index = FIELD_GET(YT921X_FDB_RESULT_INDEX_M, val);

	res = yt921x_smi_read(priv, YT921X_FDB_OUT1, &data1);
	if (res)
		return res;
	if ((data1 & YT921X_FDB_IO1_STATUS_M) ==
	    YT921X_FDB_IO1_STATUS_INVALID) {
		*ports_maskp = 0;
		return 0;
	}

	res = yt921x_smi_read(priv, YT921X_FDB_OUT0, &data0);
	if (res)
		return res;
	res = yt921x_smi_read(priv, YT921X_FDB_OUT2, &data2);
	if (res)
		return res;

	addr[0] = data0 >> 24;
	addr[1] = data0 >> 16;
	addr[2] = data0 >> 8;
	addr[3] = data0;
	addr[4] = data1 >> 8;
	addr[5] = data1;
	*vidp = FIELD_GET(YT921X_FDB_IO1_FID_M, data1);
	*indexp = index;
	*ports_maskp = FIELD_GET(YT921X_FDB_IO2_EGR_PORTS_M, data2);
	*statusp = FIELD_GET(YT921X_FDB_IO1_STATUS_M, data1);

	dev_dbg(dev,
		"%s: index 0x%x, mac %02x:%02x:%02x:%02x:%02x:%02x, "
		"vid %d, ports 0x%x, status %d\n",
		__func__, *indexp, addr[0], addr[1], addr[2], addr[3],
		addr[4], addr[5], *vidp, *ports_maskp, *statusp);
	return 0;
}

static int
yt921x_fdb_dump(struct yt921x_priv *priv, u16 ports_mask,
		dsa_fdb_dump_cb_t *cb, void *data)
{
	unsigned char addr[ETH_ALEN];
	u8 status;
	u16 pmask;
	u16 index;
	u32 ctrl;
	u16 vid;
	int res;

	ctrl = YT921X_FDB_OP_INDEX(0) | YT921X_FDB_OP_MODE_INDEX |
	       YT921X_FDB_OP_OP_GET_ONE | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;
	res = yt921x_fdb_read(priv, addr, &vid, &pmask, &index, &status);
	if (res)
		return res;
	if ((pmask & ports_mask) != 0 && (addr[0] & 1) == 0) {
		res = cb(addr, vid,
			 status == YT921X_FDB_ENTRY_STATUS_STATIC, data);
		if (res)
			return res;
	}

	ctrl = YT921X_FDB_IO2_EGR_PORTS(ports_mask);
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, ctrl);
	if (res)
		return res;

	index = 0;
	do {
		ctrl = YT921X_FDB_OP_INDEX(index) | YT921X_FDB_OP_MODE_INDEX |
		       YT921X_FDB_OP_NEXT_TYPE_UCAST_PORT |
		       YT921X_FDB_OP_OP_GET_NEXT | YT921X_FDB_OP_START;
		res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
		if (res)
			return res;

		res = yt921x_fdb_read(priv, addr, &vid, &pmask, &index,
				      &status);
		if (res)
			return res;
		if (!pmask)
			break;

		if ((pmask & ports_mask) != 0 && (addr[0] & 1) == 0) {
			res = cb(addr, vid,
				 status == YT921X_FDB_ENTRY_STATUS_STATIC,
				 data);
			if (res)
				return res;
		}

		/* Never call GET_NEXT with 4095, otherwise it will hang
		 * forever until a reset!
		 */
	} while (index < YT921X_FDB_NUM - 1);

	return 0;
}

static int
yt921x_mdb_dump(struct yt921x_priv *priv, u16 ports_mask,
		dsa_fdb_dump_cb_t *cb, void *data)
{
	unsigned char addr[ETH_ALEN];
	u8 status;
	u16 pmask;
	u16 index;
	u32 ctrl;
	u16 vid;
	int res;

	ctrl = YT921X_FDB_OP_INDEX(0) | YT921X_FDB_OP_MODE_INDEX |
	       YT921X_FDB_OP_OP_GET_ONE | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;
	res = yt921x_fdb_read(priv, addr, &vid, &pmask, &index, &status);
	if (res)
		return res;
	if ((pmask & ports_mask) != 0 && (addr[0] & 1) != 0) {
		res = cb(addr, vid,
			 status == YT921X_FDB_ENTRY_STATUS_STATIC, data);
		if (res)
			return res;
	}

	index = 0;
	do {
		ctrl = YT921X_FDB_OP_INDEX(index) | YT921X_FDB_OP_MODE_INDEX |
		       YT921X_FDB_OP_NEXT_TYPE_MCAST |
		       YT921X_FDB_OP_OP_GET_NEXT | YT921X_FDB_OP_START;
		res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
		if (res)
			return res;

		res = yt921x_fdb_read(priv, addr, &vid, &pmask, &index,
				      &status);
		if (res)
			return res;
		if (!pmask)
			break;

		if ((pmask & ports_mask) != 0 && (addr[0] & 1) != 0) {
			res = cb(addr, vid,
				 status == YT921X_FDB_ENTRY_STATUS_STATIC,
				 data);
			if (res)
				return res;
		}

		/* Never call GET_NEXT with 4095, otherwise it will hang
		 * forever until a reset!
		 */
	} while (index < YT921X_FDB_NUM - 1);

	return 0;
}

static int
yt921x_fdb_flush(struct yt921x_priv *priv, u16 ports_mask, u16 vid,
		 bool flush_static)
{
	u32 ctrl;
	u32 val;
	int res;

	if (vid) {
		ctrl = YT921X_FDB_IO1_FID(vid);
		res = yt921x_smi_write(priv, YT921X_FDB_IN1, ctrl);
		if (res)
			return res;
	}

	ctrl = YT921X_FDB_IO2_EGR_PORTS(ports_mask);
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, ctrl);
	if (res)
		return res;

	ctrl = YT921X_FDB_OP_OP_FLUSH | YT921X_FDB_OP_START;
	if (vid)
		ctrl |= YT921X_FDB_OP_FLUSH_PORT;
	else
		ctrl |= YT921X_FDB_OP_FLUSH_PORT_VID;
	if (flush_static)
		ctrl |= YT921X_FDB_OP_FLUSH_STATIC;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;

	return 0;
}

static int yt921x_fdb_del_index(struct yt921x_priv *priv, u16 index)
{
	u32 ctrl;
	u32 val;
	int res;

	ctrl = YT921X_FDB_OP_INDEX(index) | YT921X_FDB_OP_MODE_INDEX |
	       YT921X_FDB_OP_OP_DEL | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;

	return 0;
}

static int yt921x_fdb_add_index(struct yt921x_priv *priv, u16 index)
{
	u32 ctrl;
	u32 val;
	int res;

	ctrl = YT921X_FDB_OP_INDEX(index) | YT921X_FDB_OP_MODE_INDEX |
	       YT921X_FDB_OP_OP_ADD | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;

	return 0;
}

static int
yt921x_fdb_del(struct yt921x_priv *priv, const unsigned char *addr, u16 vid)
{
	u32 ctrl;
	u32 val;
	int res;

	ctrl = 0;
	res = yt921x_fdb_in01(priv, addr, vid, ctrl);
	if (res)
		return res;

	ctrl = YT921X_FDB_OP_OP_DEL | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;

	return 0;
}

static int
yt921x_fdb_add(struct yt921x_priv *priv, const unsigned char *addr, u16 vid,
	       u16 ports_mask)
{
	u32 ctrl;
	u32 val;
	int res;

	ctrl = YT921X_FDB_IO1_STATUS_STATIC;
	res = yt921x_fdb_in01(priv, addr, vid, ctrl);
	if (res)
		return res;

	ctrl = YT921X_FDB_IO2_EGR_PORTS(ports_mask);
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, ctrl);
	if (res)
		return res;

	ctrl = YT921X_FDB_OP_OP_ADD | YT921X_FDB_OP_START;
	res = yt921x_smi_write(priv, YT921X_FDB_OP, ctrl);
	if (res)
		return res;

	res = yt921x_fdb_wait(priv, &val);
	if (res)
		return res;

	return 0;
}

static int
yt921x_fdb_leave(struct yt921x_priv *priv, const unsigned char *addr,
		 u16 vid, u16 ports_mask)
{
	u16 index;
	u32 data1;
	u32 data2;
	u32 val;
	int res;

	/* Check for presence */
	res = yt921x_fdb_has(priv, addr, vid, &index);
	if (res)
		return res;
	if (index >= YT921X_FDB_NUM)
		return 0;

	/* Check if action required */
	res = yt921x_smi_read(priv, YT921X_FDB_OUT2, &data2);
	if (res)
		return res;
	val = data2 & ~YT921X_FDB_IO2_EGR_PORTS(ports_mask);
	if (val == data2)
		return 0;
	if ((val & YT921X_FDB_IO2_EGR_PORTS_M) == 0)
		return yt921x_fdb_del_index(priv, index);
	data2 = val;

	/* Overwrite entry */
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, data2);
	if (res)
		return res;

	res = yt921x_smi_read(priv, YT921X_FDB_OUT1, &data1);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_FDB_IN1, data1);
	if (res)
		return res;

	return yt921x_fdb_add_index(priv, index);
}

static int
yt921x_fdb_join(struct yt921x_priv *priv, const unsigned char *addr, u16 vid,
		u16 ports_mask)
{
	u16 index;
	u32 data1;
	u32 data2;
	u32 val;
	int res;

	/* Check for presence */
	res = yt921x_fdb_has(priv, addr, vid, &index);
	if (res)
		return res;
	if (index >= YT921X_FDB_NUM)
		return yt921x_fdb_add(priv, addr, vid, ports_mask);

	/* Check if action required */
	res = yt921x_smi_read(priv, YT921X_FDB_OUT2, &data2);
	if (res)
		return res;
	val = data2 | YT921X_FDB_IO2_EGR_PORTS(ports_mask);
	if (val == data2)
		return 0;
	data2 = val;

	/* Overwrite entry */
	res = yt921x_smi_write(priv, YT921X_FDB_IN2, data2);
	if (res)
		return res;

	res = yt921x_smi_read(priv, YT921X_FDB_OUT1, &data1);
	if (res)
		return res;
	res = yt921x_smi_write(priv, YT921X_FDB_IN1, data1);
	if (res)
		return res;

	return yt921x_fdb_add_index(priv, index);
}

static int
yt921x_dsa_port_fdb_dump(struct dsa_switch *ds, int port,
			 dsa_fdb_dump_cb_t *cb, void *data)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	do {
		res = yt921x_fdb_dump(priv, BIT(port), cb, data);
		if (res)
			break;
		res = yt921x_mdb_dump(priv, BIT(port), cb, data);
	} while (0);
	yt921x_smi_release(priv);

	return res;
}

static void yt921x_dsa_port_fast_age(struct dsa_switch *ds, int port)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	res = yt921x_fdb_flush(priv, BIT(port), 0, false);
	yt921x_smi_release(priv);

	consume_retval(res);
}

static int
yt921x_dsa_port_vlan_fast_age(struct dsa_switch *ds, int port, u16 vid)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d, vlan %d\n", __func__, port, vid);

	yt921x_smi_acquire(priv);
	res = yt921x_fdb_flush(priv, BIT(port), vid, false);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	u32 ctrl;
	int res;

	dev_dbg(dev, "%s: %d\n", __func__, msecs);

	/* AGEING reg is set in 5s step */
	ctrl = msecs / 5000;

	/* Handle case with 0 as val to NOT disable learning */
	if (!ctrl)
		ctrl = 1;
	else if (ctrl > 0xffff)
		ctrl = 0xffff;

	yt921x_smi_acquire(priv);
	res = yt921x_smi_write(priv, YT921X_AGEING, ctrl);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_fdb_del(struct dsa_switch *ds, int port,
			const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	/* assume correct port mask */
	res = yt921x_fdb_del(priv, addr, vid);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_fdb_add(struct dsa_switch *ds, int port,
			const unsigned char *addr, u16 vid, struct dsa_db db)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	res = yt921x_fdb_add(priv, addr, vid, BIT(port));
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_mdb_del(struct dsa_switch *ds, int port,
			const struct switchdev_obj_port_mdb *mdb,
			struct dsa_db db)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	const unsigned char *addr = mdb->addr;
	struct device *dev = to_device(priv);
	u16 vid = mdb->vid;
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	res = yt921x_fdb_leave(priv, addr, vid, BIT(port));
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_mdb_add(struct dsa_switch *ds, int port,
			const struct switchdev_obj_port_mdb *mdb,
			struct dsa_db db)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	const unsigned char *addr = mdb->addr;
	struct device *dev = to_device(priv);
	u16 vid = mdb->vid;
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	res = yt921x_fdb_join(priv, addr, vid, BIT(port));
	yt921x_smi_release(priv);

	return res;
}

/******** port ********/

static int
yt921x_port_config(struct yt921x_priv *priv, int port, unsigned int mode,
		   const struct phylink_link_state *state)
{
	const struct yt921x_info *info = priv->info;
	struct device *dev = to_device(priv);
	enum yt921x_fixed fixed;
	bool duplex_full;
	u32 mask;
	u32 ctrl;
	int res;

	if (state->interface != PHY_INTERFACE_MODE_INTERNAL &&
	    !yt921x_info_port_is_external(info, port)) {
		dev_err(dev, "Wrong mode %d on port %d\n",
			state->interface, port);
		return -EINVAL;
	}

	fixed = YT921X_FIXED_NOTFIXED;
	ctrl = YT921X_PORT_LINK;
	if (mode == MLO_AN_FIXED)
		switch (state->speed) {
		case 10:
			fixed = YT921X_FIXED_10;
			ctrl = YT921X_PORT_SPEED_10;
			break;
		case 100:
			fixed = YT921X_FIXED_100;
			ctrl = YT921X_PORT_SPEED_100;
			break;
		case 1000:
			fixed = YT921X_FIXED_1000;
			ctrl = YT921X_PORT_SPEED_1000;
			break;
		case 2500:
			fixed = YT921X_FIXED_2500;
			ctrl = YT921X_PORT_SPEED_2500;
			break;
		default:
			if (state->speed >= 0)
				dev_err(dev, "Unsupported speed %d\n",
					state->speed);
			break;
		}

	if (fixed != YT921X_FIXED_NOTFIXED) {
		switch (state->duplex) {
		case DUPLEX_FULL:
			duplex_full = true;
			ctrl |= YT921X_PORT_DUPLEX_FULL;
			break;
		case DUPLEX_HALF:
			duplex_full = false;
			break;
		default:
			dev_err(dev,
				"Unspecified duplex while fixed speed %d\n",
				state->speed);
			duplex_full = true;
			break;
		}
	}

	if ((state->pause & MLO_PAUSE_RX) != 0)
		ctrl |= YT921X_PORT_RX_PAUSE;
	if ((state->pause & MLO_PAUSE_TX) != 0)
		ctrl |= YT921X_PORT_TX_PAUSE;
	if ((state->pause & MLO_PAUSE_AN) != 0)
		ctrl |= YT921X_PORT_CTRL_PAUSE_AN;

	res = yt921x_smi_write(priv, YT921X_PORTn_CTRL(port), ctrl);
	if (res)
		return res;

	switch (state->interface) {
	/* SGMII */
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_100BASEX:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		res = yt921x_smi_set_bits(priv, YT921X_SGMII_CTRL,
					  YT921X_SGMII_CTRL_EXTIFn(port));
		if (res)
			return res;
		res = yt921x_smi_clear_bits(priv, YT921X_XMII_CTRL,
					    YT921X_XMII_CTRL_EXTIFn(port));
		if (res)
			return res;

		switch (state->interface) {
		case PHY_INTERFACE_MODE_SGMII:
			ctrl = YT921X_SGMII_MODE_SGMII_PHY;
			break;
		case PHY_INTERFACE_MODE_100BASEX:
			ctrl = YT921X_SGMII_MODE_100BASEX;
			break;
		case PHY_INTERFACE_MODE_1000BASEX:
			ctrl = YT921X_SGMII_MODE_1000BASEX;
			break;
		case PHY_INTERFACE_MODE_2500BASEX:
			ctrl = YT921X_SGMII_MODE_2500BASEX;
			break;
		default:
			should_unreachable();
			break;
		}
		mask = YT921X_SGMII_MODE_M;

		if (fixed != YT921X_FIXED_NOTFIXED) {
			ctrl |= YT921X_SGMII_LINK;

			switch (fixed) {
			case YT921X_FIXED_10:
				ctrl |= YT921X_SGMII_SPEED_10;
				break;
			case YT921X_FIXED_100:
				ctrl |= YT921X_SGMII_SPEED_100;
				break;
			case YT921X_FIXED_1000:
				ctrl |= YT921X_SGMII_SPEED_1000;
				break;
			case YT921X_FIXED_2500:
				ctrl |= YT921X_SGMII_SPEED_2500;
				break;
			default:
				should_unreachable();
				break;
			}

			if (duplex_full)
				ctrl |= YT921X_SGMII_DUPLEX_FULL;
			if ((state->pause & MLO_PAUSE_RX) != 0)
				ctrl |= YT921X_SGMII_RX_PAUSE;
			if ((state->pause & MLO_PAUSE_TX) != 0)
				ctrl |= YT921X_SGMII_TX_PAUSE;

			mask |= YT921X_SGMII_LINK;
			mask |= YT921X_SGMII_SPEED_M;
			mask |= YT921X_PORT_DUPLEX_FULL;
			mask |= YT921X_SGMII_RX_PAUSE;
			mask |= YT921X_SGMII_TX_PAUSE;
		}

		res = yt921x_smi_update_bits(priv, YT921X_SGMIIn(port),
					     mask, ctrl);
		if (res)
			return res;

		break;
	/* XMII */
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_REVMII:
	case PHY_INTERFACE_MODE_RMII:
	case PHY_INTERFACE_MODE_REVRMII:
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* TODO */
		dev_err(dev, "Untested mode %d\n", state->interface);
		return -EINVAL;

		res = yt921x_smi_clear_bits(priv, YT921X_SGMII_CTRL,
					    YT921X_SGMII_CTRL_EXTIFn(port));
		if (res)
			return res;
		res = yt921x_smi_set_bits(priv, YT921X_XMII_CTRL,
					  YT921X_XMII_CTRL_EXTIFn(port));
		if (res)
			return res;

		ctrl = YT921X_XMII_EN;
		switch (state->interface) {
		case PHY_INTERFACE_MODE_MII:
			ctrl |= YT921X_XMII_MODE_MII;
			break;
		case PHY_INTERFACE_MODE_REVMII:
			ctrl |= YT921X_XMII_MODE_REVMII;
			break;
		case PHY_INTERFACE_MODE_RMII:
			ctrl |= YT921X_XMII_MODE_RMII;
			break;
		case PHY_INTERFACE_MODE_REVRMII:
			ctrl |= YT921X_XMII_MODE_REVRMII;
			break;
		case PHY_INTERFACE_MODE_RGMII:
		case PHY_INTERFACE_MODE_RGMII_ID:
		case PHY_INTERFACE_MODE_RGMII_RXID:
		case PHY_INTERFACE_MODE_RGMII_TXID:
			ctrl |= YT921X_XMII_MODE_RGMII;
			break;
		default:
			should_unreachable();
			break;
		}
		mask = YT921X_XMII_EN | YT921X_XMII_MODE_M;
		res = yt921x_smi_update_bits(priv, YT921X_XMIIn(port),
					     mask, ctrl);
		if (res)
			return res;

		/* TODO: RGMII delay */

		if (fixed != YT921X_FIXED_NOTFIXED) {
			ctrl = YT921X_MDIO_POLLING_LINK;

			switch (fixed) {
			case YT921X_FIXED_10:
				ctrl |= YT921X_MDIO_POLLING_SPEED_10;
				break;
			case YT921X_FIXED_100:
				ctrl |= YT921X_MDIO_POLLING_SPEED_100;
				break;
			case YT921X_FIXED_1000:
				ctrl |= YT921X_MDIO_POLLING_SPEED_1000;
				break;
			case YT921X_FIXED_2500:
				ctrl |= YT921X_MDIO_POLLING_SPEED_2500;
				break;
			default:
				should_unreachable();
				break;
			}
			if (duplex_full)
				ctrl |= YT921X_MDIO_POLLING_DUPLEX_FULL;

			res = yt921x_smi_write(priv, YT921X_MDIO_POLLINGn(port),
					       ctrl);
			if (res)
				return res;
		}

		break;
	default:
		break;
	}

	return 0;
}

static void
yt921x_phylink_mac_link_down(struct phylink_config *config, unsigned int mode,
			     phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int port = dp->index;
	u32 ctrl;
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	ctrl = YT921X_PORT_RX_MAC_EN | YT921X_PORT_TX_MAC_EN;
	res = yt921x_smi_clear_bits_burst(priv, YT921X_PORTn_CTRL(port), ctrl);
	if (res)
		dev_err(dev, "Cannot disable port %d: %i\n", port, res);
}

static void
yt921x_phylink_mac_link_up(struct phylink_config *config,
			   struct phy_device *phydev, unsigned int mode,
			   phy_interface_t interface, int speed, int duplex,
			   bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int port = dp->index;
	u32 ctrl;
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	ctrl = YT921X_PORT_RX_MAC_EN | YT921X_PORT_TX_MAC_EN;
	res = yt921x_smi_set_bits_burst(priv, YT921X_PORTn_CTRL(port), ctrl);
	if (res)
		dev_err(dev, "Cannot enable port %d: %i\n", port, res);
}

static void
yt921x_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			  const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int port = dp->index;
	int res;

	dev_dbg(dev,
		"%s: port %d, mode %u, interface %d, speed %d, duplex %d, "
		"pause %d, advertising %lx\n", __func__,
		port, mode, state->interface, state->speed, state->duplex,
		state->pause, *state->advertising);

	yt921x_smi_acquire(priv);
	res = yt921x_port_config(priv, port, mode, state);
	yt921x_smi_release(priv);

	if (res)
		dev_err(dev, "Cannot configure port %d: %i\n", port, res);
}

static void
yt921x_dsa_phylink_get_caps(struct dsa_switch *ds, int port,
			    struct phylink_config *config)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	const struct yt921x_info *info = priv->info;

	if (yt921x_info_port_is_internal(info, port)) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		config->mac_capabilities = 0;
	} else if (yt921x_info_port_is_external(info, port)) {
		/* TODO: external port may support SGMII only, XMII only, or
		 * SGMII + XMII depending on the chip. However, we can't get
		 * the accurate config table due to lack of document, thus
		 * we simply declare SGMII + XMII and rely on the correctness
		 * of devicetree for now.
		 */

		/* SGMII */
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_100BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		config->mac_capabilities = MAC_2500FD;

		/* XMII */
		__set_bit(PHY_INTERFACE_MODE_MII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_REVMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_REVRMII,
			  config->supported_interfaces);
		phy_interface_set_rgmii(config->supported_interfaces);
	} else {
		return;
	}

	config->mac_capabilities |= MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				    MAC_10 | MAC_100 | MAC_1000;
}

static int yt921x_port_setup(struct yt921x_priv *priv, int port)
{
	u32 mask;
	u32 ctrl;
	int res;

	/* Enable port isolation */
	if ((BIT(port) & priv->cpu_ports_mask) != 0)
		ctrl = (u32)~priv->cpu_ports_mask;
	else
		ctrl = 0;
	res = yt921x_smi_write(priv, YT921X_PORTn_ISOLATION(port), ctrl);
	if (res)
		return res;

	/* Set PVID */
	mask = YT921X_PORT_VLAN_CTRL_SVID_M | YT921X_PORT_VLAN_CTRL_CVID_M;
	ctrl = YT921X_PORT_VLAN_CTRL_SVID(YT921X_PVID_DEFAULT) |
	       YT921X_PORT_VLAN_CTRL_CVID(YT921X_PVID_DEFAULT);
	res = yt921x_smi_update_bits(priv, YT921X_PORTn_VLAN_CTRL(port),
				     mask, ctrl);
	if (res)
		return res;

	return 0;
}

static enum dsa_tag_protocol
yt921x_dsa_get_tag_protocol(struct dsa_switch *ds, int port,
			    enum dsa_tag_protocol m)
{
	return DSA_TAG_PROTO_YT921X;
}

static int yt921x_dsa_port_setup(struct dsa_switch *ds, int port)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	yt921x_smi_acquire(priv);
	res = yt921x_port_setup(priv, port);
	yt921x_smi_release(priv);

	return res;
}

/******** chip ********/

static int yt921x_detect(struct yt921x_priv *priv)
{
	struct device *dev = to_device(priv);
	const struct yt921x_info *info;
	u32 chipid;
	u32 major;
	u32 mode;
	u8 extmode;
	int res;

	res = yt921x_smi_read_burst(priv, YT921X_CHIP_ID, &chipid);
	if (res)
		return res;

	major = FIELD_GET(YT921X_CHIP_ID_MAJOR, chipid);

	for (info = yt921x_infos; info->name; info++)
		if (info->major == major)
			goto found_major;

	dev_err(dev, "Unexpected chipid 0x%x\n", chipid);
	return -ENODEV;

found_major:
	yt921x_smi_acquire(priv);
	do {
		res = yt921x_smi_read(priv, YT921X_CHIP_MODE, &mode);
		if (res)
			break;
		res = yt921x_edata_read(priv, YT921X_EDATA_EXTMODE, &extmode);
	} while (0);
	yt921x_smi_release(priv);
	if (res)
		return res;

	for (; info->name; info++)
		if (info->major == major && info->mode == mode &&
		    info->extmode == extmode)
			goto found_chip;

	dev_err(dev, "Unsupported chipid 0x%x chipmode 0x%x 0x%x\n",
		chipid, mode, extmode);
	return -ENODEV;

found_chip:
	dev_info(dev,
		 "Motorcomm %s switch, chipid: 0x%x, chipmode: 0x%x 0x%x\n",
		 info->name, chipid, mode, extmode);

	res = yt921x_smi_read_burst(priv, YT921X_PON_STRAP_CAP,
				    &priv->pon_strap_cap);
	if (res)
		return res;

	priv->info = info;
	return 0;
}

static int yt921x_dsa_get_eeprom_len(struct dsa_switch *ds)
{
	return YT921X_EDATA_LEN;
}

static int
yt921x_dsa_get_eeprom(struct dsa_switch *ds, struct ethtool_eeprom *eeprom,
		      u8 *data)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	unsigned int i = 0;
	int res;

	yt921x_smi_acquire(priv);
	do {
		res = yt921x_edata_wait(priv);
		if (res)
			break;
		for (; i < eeprom->len; i++) {
			unsigned int offset = eeprom->offset + i;

			res = yt921x_edata_read_cont(priv, offset, &data[i]);
			if (res)
				break;
		}
	} while (0);
	yt921x_smi_release(priv);

	eeprom->len = i;
	return res;
}

static struct dsa_port *
yt921x_dsa_preferred_default_local_cpu_port(struct dsa_switch *ds)
{
	struct dsa_port *ext_dp = NULL;
	struct dsa_port *cpu_dp;

	dsa_switch_for_each_cpu_port(cpu_dp, ds)
		if (yt921x_port_is_external(cpu_dp->index)) {
			ext_dp = cpu_dp;
			break;
		}

	return ext_dp;
}

static void
yt921x_dsa_conduit_state_change(struct dsa_switch *ds,
				const struct net_device *conduit,
				bool operational)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct dsa_port *cpu_dp = conduit->dsa_ptr;
	struct device *dev = to_device(priv);
	int port = cpu_dp->index;
	u32 ctrl;
	int res;

	dev_dbg(dev, "%s: port %d, up %d\n", __func__, port, operational);

	if (operational)
		priv->active_cpu_ports_mask |= BIT(port);
	else
		priv->active_cpu_ports_mask &= ~BIT(port);

	if (priv->active_cpu_ports_mask) {
		ctrl = YT921X_EXT_CPU_PORT_TAG_EN |
		       YT921X_EXT_CPU_PORT_PORT_EN |
		       YT921X_EXT_CPU_PORT_PORT(__fls(priv->active_cpu_ports_mask));
		res = yt921x_smi_write_burst(priv, YT921X_EXT_CPU_PORT, ctrl);
		consume_retval(res);
	}
}

static int yt921x_dsa_setup(struct dsa_switch *ds)
{
	struct yt921x_priv *priv = to_yt921x_priv(ds);
	struct device *dev = to_device(priv);
	struct device_node *np = dev->of_node;
	struct device_node *child;
	u32 ctrl;
	u32 val;
	int res;

	priv->cpu_ports_mask = dsa_cpu_ports(ds);
	if (!priv->cpu_ports_mask) {
		dev_err(dev, "No CPU port\n");
		return -ENODEV;
	}

	res = yt921x_detect(priv);
	if (res)
		return res;

	/* Reset */
	res = yt921x_smi_write_burst(priv, YT921X_RST, YT921X_RST_HW);
	if (res)
		return res;

	/* YT921X_RST_HW is almost same as GPIO hard reset, so we need
	 * this delay.
	 */
	fsleep(YT921X_RST_DELAY_US);

	res = read_poll_timeout(yt921x_smi_read_burst, res, val == 0,
				YT921X_MDIO_SLEEP_US, YT921X_RST_TIMEOUT_US,
				false, priv, YT921X_RST, &val);
	if (res) {
		dev_err(dev, "Reset timeout\n");
		return res;
	}

	/* Always register one mdio bus for the internal/default mdio bus. This
	 * maybe represented in the device tree, but is optional.
	 */
	child = of_get_child_by_name(np, "mdio");
	res = yt921x_mbus_int_init(priv, child);
	of_node_put(child);
	if (res)
		return res;
	ds->user_mii_bus = priv->mbus_int;

	/* Walk the device tree, and see if there are any other nodes which say
	 * they are compatible with the external mdio bus.
	 */
	child = of_get_child_by_name(np, "mdio-external");
	if (!child) {
		priv->mbus_ext = NULL;
	} else {
		res = yt921x_mbus_ext_init(priv, child);
		of_node_put(child);
		if (res)
			return res;

		dev_err(dev, "Untested external mdio bus\n");
		return -ENODEV;
	}

	yt921x_smi_acquire(priv);
	do {
		/* Enable DSA */
		res = yt921x_smi_read(priv, YT921X_CPU_TAG_TPID, &val);
		if (res)
			break;
		priv->tag_eth_p = FIELD_GET(YT921X_CPU_TAG_TPID_TPID_M, val);
		if (priv->tag_eth_p != YT921X_CPU_TAG_TPID_TPID_DEFAULT) {
			dev_warn(dev, "Tag type 0x%x != 0x%x\n",
				 priv->tag_eth_p,
				 YT921X_CPU_TAG_TPID_TPID_DEFAULT);
			/* Although CPU_TAG_TPID could be configured, we choose
			 * not to do so, since there is no way it could be
			 * different from the default, unless you are using the
			 * wrong chip.
			 */
			res = -EINVAL;
			break;
		}

		/* Unconditionally bring up one CPU port, to avoid warnings from
		 * yt921x_tag_rcv() before conduit_state_change() is called.
		 */
		ctrl = YT921X_EXT_CPU_PORT_TAG_EN |
		       YT921X_EXT_CPU_PORT_PORT_EN |
		       YT921X_EXT_CPU_PORT_PORT(__fls(priv->cpu_ports_mask));
		res = yt921x_smi_write(priv, YT921X_EXT_CPU_PORT, ctrl);
		if (res)
			break;

		/* Enable MIB */
		res = yt921x_smi_set_bits(priv, YT921X_FUNC, YT921X_FUNC_MIB);
		if (res)
			break;

		ctrl = YT921X_MIB_CTRL_CLEAN | YT921X_MIB_CTRL_ALL_PORT;
		res = yt921x_smi_write(priv, YT921X_MIB_CTRL, ctrl);
		if (res)
			break;

		/* Port flags */
		ctrl = ~0;
		res = yt921x_smi_write(priv, YT921X_FILTER_UNK_UCAST, ctrl);
		if (res)
			break;

		ctrl = YT921X_CPU_COPY_TO_EXT_CPU;
		res = yt921x_smi_write(priv, YT921X_CPU_COPY, ctrl);
		if (res)
			break;

		/* Temperature sensor */
		res = yt921x_smi_set_bits(priv, YT921X_SENSOR,
					  YT921X_SENSOR_TEMP);
	} while (0);
	yt921x_smi_release(priv);
	if (res)
		return res;

	return 0;
}

static const struct phylink_mac_ops yt921x_phylink_mac_ops = {
	.mac_link_down	= yt921x_phylink_mac_link_down,
	.mac_link_up	= yt921x_phylink_mac_link_up,
	.mac_config	= yt921x_phylink_mac_config,
};

static const struct dsa_switch_ops yt921x_dsa_switch_ops = {
	/* mib */
	.get_strings		= yt921x_dsa_get_strings,
	.get_ethtool_stats	= yt921x_dsa_get_ethtool_stats,
	.get_sset_count		= yt921x_dsa_get_sset_count,
	.get_stats64		= yt921x_dsa_get_stats64,
	.get_pause_stats	= yt921x_dsa_get_pause_stats,
	/* eee */
	.support_eee		= yt921x_dsa_support_eee,
	.set_mac_eee		= yt921x_dsa_set_mac_eee,
	/* mtu */
	.port_change_mtu	= yt921x_dsa_port_change_mtu,
	.port_max_mtu		= yt921x_dsa_port_max_mtu,
	/* mirror */
	.port_mirror_del	= yt921x_dsa_port_mirror_del,
	.port_mirror_add	= yt921x_dsa_port_mirror_add,
	/* vlan */
	.port_vlan_filtering	= yt921x_dsa_port_vlan_filtering,
	.port_vlan_del		= yt921x_dsa_port_vlan_del,
	.port_vlan_add		= yt921x_dsa_port_vlan_add,
	/* bridge */
	.port_pre_bridge_flags	= yt921x_dsa_port_pre_bridge_flags,
	.port_bridge_flags	= yt921x_dsa_port_bridge_flags,
	.port_bridge_leave	= yt921x_dsa_port_bridge_leave,
	.port_bridge_join	= yt921x_dsa_port_bridge_join,
	/* fdb */
	.port_fdb_dump		= yt921x_dsa_port_fdb_dump,
	.port_fast_age		= yt921x_dsa_port_fast_age,
	.port_vlan_fast_age	= yt921x_dsa_port_vlan_fast_age,
	.set_ageing_time	= yt921x_dsa_set_ageing_time,
	.port_fdb_del		= yt921x_dsa_port_fdb_del,
	.port_fdb_add		= yt921x_dsa_port_fdb_add,
	.port_mdb_del		= yt921x_dsa_port_mdb_del,
	.port_mdb_add		= yt921x_dsa_port_mdb_add,
	/* port */
	.get_tag_protocol	= yt921x_dsa_get_tag_protocol,
	.phylink_get_caps	= yt921x_dsa_phylink_get_caps,
	.port_setup		= yt921x_dsa_port_setup,
	/* chip */
	.get_eeprom_len		= yt921x_dsa_get_eeprom_len,
	.get_eeprom		= yt921x_dsa_get_eeprom,
	.preferred_default_local_cpu_port	= yt921x_dsa_preferred_default_local_cpu_port,
	.conduit_state_change	= yt921x_dsa_conduit_state_change,
	.setup			= yt921x_dsa_setup,
};

/******** debug ********/

static ssize_t
reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct yt921x_priv *priv = dev_get_drvdata(dev);

	if (!priv->reg_valid)
		return sprintf(buf, "0x%x: -\n", priv->reg_addr);

	return sprintf(buf, "0x%x: 0x%08x\n", priv->reg_addr, priv->reg_val);
}

/* Convenience sysfs attribute to read/write switch internal registers, since
 * user-space tools cannot gain exclusive access to the device, which is
 * required for any register operations.
 */
static ssize_t
reg_store(struct device *dev, struct device_attribute *attr,
	  const char *buf, size_t count)
{
	struct yt921x_priv *priv = dev_get_drvdata(dev);
	const char *end = buf + count;
	const char *p = buf;
	bool is_write;
	u32 reg;
	u32 val;
	int res;

	do {
		unsigned long v;
		char *e;

		while (p < end && isspace(*p))
			p++;
		if (p >= end)
			return -EINVAL;

		v = simple_strntoul(p, &e, 0, end - p);
		if (v >= YT921X_REG_END)
			return -EPERM;
		reg = v;
		is_write = false;

		p = e;
		if (p >= end)
			break;
		if (!isspace(*p))
			return -EINVAL;

		do
			p++;
		while (p < end && isspace(*p));
		if (p >= end)
			break;

		v = simple_strntoul(p, &e, 0, end - p);
		if ((u32)v != v)
			return -EINVAL;
		val = v;
		is_write = true;

		p = e;
		if (p >= end)
			break;
		if (!isspace(*p))
			return -EINVAL;
	} while (0);

	yt921x_smi_acquire(priv);
	if (!is_write)
		res = yt921x_smi_read(priv, reg, &val);
	else
		res = yt921x_smi_write(priv, reg, val);
	yt921x_smi_release(priv);

	if (res) {
		dev_err(dev, "Cannot access register 0x%x: %i\n", reg, res);
		return -EIO;
	}

	priv->reg_addr = reg;
	priv->reg_val = val;
	priv->reg_valid = !is_write;

	return count;
}

static DEVICE_ATTR_RW(reg);

static struct attribute *yt921x_attrs[] = {
	&dev_attr_reg.attr,
	NULL,
};

ATTRIBUTE_GROUPS(yt921x);

/******** device ********/

static void yt921x_mdio_shutdown(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct yt921x_priv *priv = dev_get_drvdata(dev);
	struct dsa_switch *ds = &priv->ds;

	dsa_switch_shutdown(ds);
}

static void yt921x_mdio_remove(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct yt921x_priv *priv = dev_get_drvdata(dev);
	struct dsa_switch *ds = &priv->ds;

	dsa_unregister_switch(ds);
}

static int yt921x_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct yt921x_smi_mdio *mdio;
	struct yt921x_priv *priv;
	struct dsa_switch *ds;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mdio = devm_kzalloc(dev, sizeof(*mdio), GFP_KERNEL);
	if (!mdio)
		return -ENOMEM;

	mdio->bus = mdiodev->bus;
	mdio->addr = mdiodev->addr;
	mdio->switchid = 0;

	priv->smi_ops = &yt921x_smi_ops_mdio;
	priv->smi_ctx = mdio;

	ds = &priv->ds;
	ds->dev = dev;
	ds->configure_vlan_while_not_filtering = true;
	ds->assisted_learning_on_cpu_port = true;
	ds->priv = priv;
	ds->ops = &yt921x_dsa_switch_ops;
	ds->phylink_mac_ops = &yt921x_phylink_mac_ops;
	ds->num_ports = YT921X_PORT_NUM;

	dev_set_drvdata(dev, priv);

	return dsa_register_switch(ds);
}

static const struct of_device_id yt921x_of_match[] = {
	{ .compatible = "motorcomm,yt9215" },
	{}
};
MODULE_DEVICE_TABLE(of, yt921x_of_match);

static struct mdio_driver yt921x_mdio_driver = {
	.probe = yt921x_mdio_probe,
	.remove = yt921x_mdio_remove,
	.shutdown = yt921x_mdio_shutdown,
	.mdiodrv.driver = {
		.name = YT921X_NAME,
		.of_match_table = yt921x_of_match,
		.dev_groups = yt921x_groups,
	},
};

mdio_module_driver(yt921x_mdio_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("Driver for Motorcomm YT921x Switch");
MODULE_LICENSE("GPL");
