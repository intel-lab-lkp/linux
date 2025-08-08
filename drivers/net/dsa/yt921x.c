// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Motorcomm YT921x Switch
 *
 * Should work on YT9213/YT9214/YT9215/YT9218, but only tested on YT9215+SGMII,
 * be sure to do your own checks before porting to another chip.
 *
 * Copyright (c) 2025 David Yang
 */

#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <net/dsa.h>

#undef dev_dbg
#define dev_dbg dev_info

/******** hardware definitions ********/

#define YT921X_SMI_SWITCHIDf		GENMASK(3, 2)
#define  YT921X_SMI_SWITCHIDv(x)		FIELD_PREP(YT921X_SMI_SWITCHIDf, (x))
#define YT921X_SMI_ADf			BIT(1)
#define  YT921X_SMI_ADDRv			0
#define  YT921X_SMI_DATAv			YT921X_SMI_ADf
#define YT921X_SMI_RWf			BIT(0)
#define  YT921X_SMI_WRITEv			0
#define  YT921X_SMI_READv			YT921X_SMI_RWf

#define YT921X_RESETm			0x80000
#define  YT921X_RESET_HWf			BIT(31)
#define  YT921X_RESET_SWf			BIT(1)
#define YT921X_FUNCm			0x80004
#define  YT921X_FUNC_MIBf			BIT(1)
#define YT921X_CHIP_IDm			0x80008
#define  YT921X_CHIP_ID_IDf			GENMASK(31, 16)
#define YT921X_EXT_CPU_PORTm		0x8000c
#define  YT921X_EXT_CPU_PORT_TAG_ENf		BIT(15)
#define  YT921X_EXT_CPU_PORT_PORT_ENf		BIT(14)
#define  YT921X_EXT_CPU_PORT_PORTf		GENMASK(3, 0)
#define   YT921X_EXT_CPU_PORT_PORTv(x)			FIELD_PREP(YT921X_EXT_CPU_PORT_PORTf, (x))
#define YT921X_CPU_TAG_TPIDm		0x80010
#define  YT921X_CPU_TAG_TPID_TPIDf		GENMASK(15, 0)
#define   YT921X_CPU_TAG_TPID_TPIDv			0x9988
#define YT921X_EXTIF_SERDESm		0x80028
#define  YT921X_EXTIF_SERDES_ENf		BIT(6)
#define  YT921X_EXTIF_SERDES_EXTIFnf(port)	BIT((port) - 8)
#define YT921X_SGMIInm(port)		(0x8008c + 4 * ((port) - 8))
#define  YT921X_SGMII_MODEf			GENMASK(9, 7)
#define   YT921X_SGMII_MODEv(x)				FIELD_PREP(YT921X_SGMII_MODEf, (x))
#define   YT921X_SGMII_MODE_SGMII_MACv			YT921X_SGMII_MODEv(0)
#define   YT921X_SGMII_MODE_SGMII_PHYv			YT921X_SGMII_MODEv(1)
#define   YT921X_SGMII_MODE_1000BASEXv			YT921X_SGMII_MODEv(2)
#define   YT921X_SGMII_MODE_100BASEXv			YT921X_SGMII_MODEv(3)
#define   YT921X_SGMII_MODE_2500BASEXv			YT921X_SGMII_MODEv(4)
#define   YT921X_SGMII_MODE_BASEXv			YT921X_SGMII_MODEv(5)
#define   YT921X_SGMII_MODE_DISABLEv			YT921X_SGMII_MODEv(6)
#define  YT921X_SGMII_RX_PAUSEf			BIT(6)
#define  YT921X_SGMII_TX_PAUSEf			BIT(5)
#define  YT921X_SGMII_LINKf			BIT(4)  /* force link */
#define  YT921X_SGMII_DUPLEX_FULLf		BIT(3)
#define  YT921X_SGMII_SPEEDf			GENMASK(2, 0)
#define   YT921X_SGMII_SPEEDv(x)			FIELD_PREP(YT921X_SGMII_SPEEDf, (x))
#define   YT921X_SGMII_SPEED_10v			YT921X_SGMII_SPEEDv(0)
#define   YT921X_SGMII_SPEED_100v			YT921X_SGMII_SPEEDv(1)
#define   YT921X_SGMII_SPEED_1000v			YT921X_SGMII_SPEEDv(2)
#define   YT921X_SGMII_SPEED_2500v			YT921X_SGMII_SPEEDv(4)
#define YT921X_PORTn_CTRLm(port)	(0x80100 + 4 * (port))
#define  YT921X_PORT_CTRL_FLOW_CONTROL_ANf	BIT(10)
#define YT921X_PORTn_STATUSm(port)	(0x80200 + 4 * (port))
#define  YT921X_PORT_LINKf			BIT(9)  /* CTRL: auto negotiation */
#define  YT921X_PORT_HALF_FLOW_CONTROLf		BIT(8)  /* Half-duplex back pressure mode */
#define  YT921X_PORT_DUPLEX_FULLf		BIT(7)
#define  YT921X_PORT_RX_FLOW_CONTROLf		BIT(6)
#define  YT921X_PORT_TX_FLOW_CONTROLf		BIT(5)
#define  YT921X_PORT_RX_MAC_ENf			BIT(4)
#define  YT921X_PORT_TX_MAC_ENf			BIT(3)
#define  YT921X_PORT_SPEEDf			GENMASK(2, 0)
#define   YT921X_PORT_SPEEDv(x)				FIELD_PREP(YT921X_PORT_SPEEDf, (x))
#define   YT921X_PORT_SPEED_10v				YT921X_PORT_SPEEDv(0)
#define   YT921X_PORT_SPEED_100v			YT921X_PORT_SPEEDv(1)
#define   YT921X_PORT_SPEED_1000v			YT921X_PORT_SPEEDv(2)
#define   YT921X_PORT_SPEED_2500v			YT921X_PORT_SPEEDv(4)
#define YT921X_PON_STRAP_FUNCm		0x80320
#define YT921X_PON_STRAP_VALm		0x80324
#define YT921X_PON_STRAP_CAPm		0x80328
#define  YT921X_PON_STRAP_EEEf			BIT(16)
#define  YT921X_PON_STRAP_LOOP_DETECTf		BIT(7)
#define YT921X_MDIO_POLLINGnm(port)	(0x80364 + 4 * ((port) - 8))
#define  YT921X_MDIO_POLLING_DUPLEX_FULLf	BIT(4)
#define  YT921X_MDIO_POLLING_LINKf		BIT(3)
#define  YT921X_MDIO_POLLING_SPEEDf		GENMASK(2, 0)
#define   YT921X_MDIO_POLLING_SPEEDv(x)			FIELD_PREP(YT921X_MDIO_POLLING_SPEEDf, (x))
#define   YT921X_MDIO_POLLING_SPEED_10v			YT921X_MDIO_POLLING_SPEEDv(0)
#define   YT921X_MDIO_POLLING_SPEED_100v		YT921X_MDIO_POLLING_SPEEDv(1)
#define   YT921X_MDIO_POLLING_SPEED_1000v		YT921X_MDIO_POLLING_SPEEDv(2)
#define   YT921X_MDIO_POLLING_SPEED_2500v		YT921X_MDIO_POLLING_SPEEDv(4)
#define YT921X_CHIP_MODEm		0x80388
#define  YT921X_CHIP_MODE_MODEf			GENMASK(1, 0)
#define YT921X_EXTIF_SELm		0x80394
#define  YT921X_EXTIF_SEL_EXTIFn_XMIIf(port)	BIT(9 - (port))  /* Yes, it's reversed */
#define YT921X_EXTIFn_MODEm(port)	(0x80400 + 8 * ((port) - 8))
#define  YT921X_EXTIF_MODE_MODEf		GENMASK(31, 29)
#define   YT921X_EXTIF_MODE_MODEv(x)			FIELD_PREP(YT921X_EXTIF_MODE_MODEf, (x))
#define   YT921X_EXTIF_MODE_MODE_MIIv			YT921X_EXTIF_MODE_MODEv(0)
#define   YT921X_EXTIF_MODE_MODE_REVMIIv		YT921X_EXTIF_MODE_MODEv(1)
#define   YT921X_EXTIF_MODE_MODE_RMIIv			YT921X_EXTIF_MODE_MODEv(2)
#define   YT921X_EXTIF_MODE_MODE_REVRMIIv		YT921X_EXTIF_MODE_MODEv(3)
#define   YT921X_EXTIF_MODE_MODE_RGMIIv			YT921X_EXTIF_MODE_MODEv(4)
#define   YT921X_EXTIF_MODE_MODE_DISABLEv		YT921X_EXTIF_MODE_MODEv(5)
#define  YT921X_EXTIF_MODE_PORT_ENf		BIT(18)

#define YT921X_MACn_JUMBOm(port)	(0x81008 + 0x1000 * (port))
#define  YT921X_MAC_FRAME_SIZEf			GENMASK(21, 8)
#define   YT921X_MAC_FRAME_SIZEv(x)			FIELD_PREP(YT921X_MAC_FRAME_SIZEf, (x))

#define YT921X_MACn_EEE_VALm(port)	(0xa0000 + 4 * (port))
#define  YT921X_MAC_EEE_VAL_DATAf		BIT(1)

#define YT921X_MAC_EEE_CTRLm		0xb0000
#define  YT921X_MAC_EEE_CTRL_ENnf(port)		BIT(port)

#define YT921X_MIB_CTRLm		0xc0004
#define  YT921X_MIB_CTRL_CLEANf			BIT(30)
#define  YT921X_MIB_CTRL_PORTf			GENMASK(6, 3)
#define   YT921X_MIB_CTRL_PORTv(x)			FIELD_PREP(YT921X_MIB_CTRL_PORTf, (x))
#define  YT921X_MIB_CTRL_ONE_PORTf		BIT(1)
#define  YT921X_MIB_CTRL_ALL_PORTf		BIT(0)

#define YT921X_MIBn_DATAnm(port, x)	(0xc0100 + 0x100 * (port) + 4 * (x))

#define YT921X_EDATA_CTRLm		0xe0000
#define  YT921X_EDATA_CTRL_ADDRf		GENMASK(15, 8)
#define   YT921X_EDATA_CTRL_ADDRv(x)			FIELD_PREP(YT921X_EDATA_CTRL_ADDRf, (x))
#define  YT921X_EDATA_CTRL_OPf			GENMASK(3, 0)
#define   YT921X_EDATA_CTRL_READv			5
#define YT921X_EDATA_DATAm		0xe0004
#define  YT921X_EDATA_DATA_DATAf		GENMASK(31, 24)
#define  YT921X_EDATA_DATA_STATUSf		GENMASK(3, 0)
#define   YT921X_EDATA_DATA_IDLEv			3

#define YT921X_EXTIF_MDIO_OPm		0x6a000
#define YT921X_INTIF_MDIO_OPm		0xf0000
#define  YT921X_IF_MDIO_OP_DOf			BIT(0)
#define YT921X_EXTIF_MDIO_CTRLm		0x6a004
#define YT921X_INTIF_MDIO_CTRLm		0xf0004
#define  YT921X_IF_MDIO_CTRL_PORTf		GENMASK(25, 21)
#define   YT921X_IF_MDIO_CTRL_PORTv(x)			FIELD_PREP(YT921X_IF_MDIO_CTRL_PORTf, (x))
#define  YT921X_IF_MDIO_CTRL_REGf		GENMASK(20, 16)
#define   YT921X_IF_MDIO_CTRL_REGv(x)			FIELD_PREP(YT921X_IF_MDIO_CTRL_REGf, (x))
#define  YT921X_IF_MDIO_CTRL_TYPEf		GENMASK(11, 8)
#define   YT921X_IF_MDIO_CTRL_TYPEv(x)			FIELD_PREP(YT921X_IF_MDIO_CTRL_TYPEf, (x))
#define   YT921X_IF_MDIO_CTRL_TYPE_C22v			YT921X_IF_MDIO_CTRL_TYPEv(4)
#define  YT921X_IF_MDIO_CTRL_OPf		GENMASK(3, 2)
#define   YT921X_IF_MDIO_CTRL_OPv(x)			FIELD_PREP(YT921X_IF_MDIO_CTRL_OPf, (x))
#define   YT921X_IF_MDIO_CTRL_WRITEv			YT921X_IF_MDIO_CTRL_OPv(1)
#define   YT921X_IF_MDIO_CTRL_READv			YT921X_IF_MDIO_CTRL_OPv(2)
#define YT921X_EXTIF_MDIO_WRITE_DATAm	0x6a008
#define YT921X_INTIF_MDIO_WRITE_DATAm	0xf0008
#define YT921X_EXTIF_MDIO_READ_DATAm	0x6a00c
#define YT921X_INTIF_MDIO_READ_DATAm	0xf000c

#define YT921X_EDATA_EXTMODEm		0xfb

#define YT921X_FRAME_SIZE_MAX		0x2400  /* 9216 */
#define YT921X_TAG_LEN			8

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

#define u64_from_u32(hi, lo) (((u64)(hi) << 32) | (lo))

struct yt921x_chip_info {
	const char *name;
	u16 id;
	u8 mode;
	u8 extmode;
	u16 intif_mask;
	u16 extif_mask;
};

static const struct yt921x_chip_info yt921x_chip_infos[] = {
	{ "YT9215SC", 0x9002, 1, 0, GENMASK(4, 0),   BIT(8) | BIT(9), },
	{ "YT9215S",  0x9002, 2, 0, GENMASK(4, 0),   BIT(8),          },
	{ "YT9215RB", 0x9002, 3, 0, GENMASK(4, 0),   BIT(8) | BIT(9), },
	{ "YT9214NB", 0x9002, 3, 2, BIT(1) | BIT(3), BIT(8) | BIT(9), },
	{ "YT9213NB", 0x9002, 3, 3, BIT(1) | BIT(3), BIT(9),          },
	{ "YT9218N",  0x9001, 0, 0, GENMASK(7, 0),   0,               },
	{ "YT9218MB", 0x9001, 1, 0, GENMASK(7, 0),   BIT(8) | BIT(9), },
	{}
};

#define YT921X_INT_PORTS_NUM 8
#define YT921X_EXT_PORTS_NUM 2
/* 8 internal + 2 external + 1 mcu */
#define YT921X_PORT_CTRLS_NUM 11

/******** driver definitions ********/

#define YT921X_MDIO_SLEEP_US	10000
#define YT921X_MDIO_TIMEOUT_US	100000
#define YT921X_RESET_TIMEOUT_US 100000

enum yt921x_speed {
	YT921X_SPEED_AN = 0,
	YT921X_SPEED_10,
	YT921X_SPEED_100,
	YT921X_SPEED_1000,
	YT921X_SPEED_2500,
};

struct yt921x_smi_ops {
	int (*acquire)(void *context);
	void (*release)(void *context);
	int (*read)(void *context, u32 reg, u32 *valp);
	int (*write)(void *context, u32 reg, u32 val);
};

struct yt921x_smi_mdio {
	struct mii_bus *bus;
	int addr;
	unsigned char switchid;
};

/* TODO: SPI/I2C */

struct yt921x_port {
	struct yt921x_mib_raw mib;
};

struct yt921x_priv {
	struct device *dev;
	const struct yt921x_chip_info *info;
	u32 pon_strap_cap;
	u16 tag_eth_p;

	const struct yt921x_smi_ops *smi_ops;
	void *smi_ctx;

	/* mdio master */
	struct mii_bus *mbus_int;
	struct mii_bus *mbus_ext;

	struct dsa_switch ds;

	struct yt921x_port ports[YT921X_PORT_CTRLS_NUM];

	u16 eee_ports_mask;
};

/******** smi ********/

static int yt921x_smi_acquire(struct yt921x_priv *priv)
{
	if (priv->smi_ops->acquire)
		return priv->smi_ops->acquire(priv->smi_ctx);
	return 0;
}

static void yt921x_smi_release(struct yt921x_priv *priv)
{
	if (priv->smi_ops->release)
		priv->smi_ops->release(priv->smi_ctx);
}

static int
__yt921x_smi_read(struct yt921x_priv *priv, u32 reg, u32 *valp)
{
	return priv->smi_ops->read(priv->smi_ctx, reg, valp);
}

static int yt921x_smi_read(struct yt921x_priv *priv, u32 reg, u32 *valp)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_read(priv, reg, valp);
	yt921x_smi_release(priv);

	return res;
}

static int
__yt921x_smi_write(struct yt921x_priv *priv, u32 reg, u32 val)
{
	return priv->smi_ops->write(priv->smi_ctx, reg, val);
}

static int yt921x_smi_write(struct yt921x_priv *priv, u32 reg, u32 val)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, reg, val);
	yt921x_smi_release(priv);

	return res;
}

static int
__yt921x_smi_update_bits(struct yt921x_priv *priv, u32 reg, u32 mask, u32 val)
{
	u32 v;
	int res;

	res = __yt921x_smi_read(priv, reg, &v);
	if (unlikely(res != 0))
		return res;
	v &= ~mask;
	v |= val;
	return __yt921x_smi_write(priv, reg, v);
}

static int
yt921x_smi_update_bits(struct yt921x_priv *priv, u32 reg, u32 mask, u32 val)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_update_bits(priv, reg, mask, val);
	yt921x_smi_release(priv);

	return res;
}

static int __yt921x_smi_set_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return __yt921x_smi_update_bits(priv, reg, 0, mask);
}

static int yt921x_smi_set_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits(priv, reg, 0, mask);
}

static int __yt921x_smi_clear_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return __yt921x_smi_update_bits(priv, reg, mask, 0);
}

static int yt921x_smi_clear_bits(struct yt921x_priv *priv, u32 reg, u32 mask)
{
	return yt921x_smi_update_bits(priv, reg, mask, 0);
}

static int
__yt921x_smi_toggle_bits(struct yt921x_priv *priv, u32 reg, u32 mask, bool set)
{
	return __yt921x_smi_update_bits(priv, reg, mask, !set ? 0 : mask);
}

/******** smi via mdio ********/

static int yt921x_smi_mdio_acquire(void *context)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	return 0;
}

static void yt921x_smi_mdio_release(void *context)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;

	mutex_unlock(&bus->mdio_lock);
}

static int yt921x_smi_mdio_read(void *context, u32 reg, u32 *valp)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;
	int addr = mdio->addr;
	u32 reg_addr = YT921X_SMI_SWITCHIDv(mdio->switchid) |
		       YT921X_SMI_ADDRv | YT921X_SMI_READv;
	u32 reg_data = YT921X_SMI_SWITCHIDv(mdio->switchid) |
		       YT921X_SMI_DATAv | YT921X_SMI_READv;
	u32 val;
	int res;

	res = __mdiobus_write(bus, addr, reg_addr, (reg >> 16) & 0xffff);
	if (unlikely(res != 0))
		return res;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >>  0) & 0xffff);
	if (unlikely(res != 0))
		return res;

	res = __mdiobus_read(bus, addr, reg_data);
	if (unlikely(res < 0))
		return res;
	val = res & 0xffff;
	res = __mdiobus_read(bus, addr, reg_data);
	if (unlikely(res < 0))
		return res;
	val = (val << 16) | (res & 0xffff);

	*valp = val;
	return 0;
}

static int yt921x_smi_mdio_write(void *context, u32 reg, u32 val)
{
	struct yt921x_smi_mdio *mdio = context;
	struct mii_bus *bus = mdio->bus;
	int addr = mdio->addr;
	u32 reg_addr = YT921X_SMI_SWITCHIDv(mdio->switchid) |
		       YT921X_SMI_ADDRv | YT921X_SMI_WRITEv;
	u32 reg_data = YT921X_SMI_SWITCHIDv(mdio->switchid) |
		       YT921X_SMI_DATAv | YT921X_SMI_WRITEv;
	int res;

	res = __mdiobus_write(bus, addr, reg_addr, (reg >> 16) & 0xffff);
	if (unlikely(res != 0))
		return res;
	res = __mdiobus_write(bus, addr, reg_addr, (reg >>  0) & 0xffff);
	if (unlikely(res != 0))
		return res;

	res = __mdiobus_write(bus, addr, reg_data, (val >> 16) & 0xffff);
	if (unlikely(res != 0))
		return res;
	res = __mdiobus_write(bus, addr, reg_data, (val >>  0) & 0xffff);
	if (unlikely(res != 0))
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

static int __yt921x_edata_out(struct yt921x_priv *priv, u32 *valp)
{
	u32 val;
	int res;

	res = __yt921x_smi_read(priv, YT921X_EDATA_DATAm, &val);
	if (unlikely(res != 0))
		return res;
	if ((val & YT921X_EDATA_DATA_STATUSf) != YT921X_EDATA_DATA_IDLEv) {
		int res2;

		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read, res,
					(val & YT921X_EDATA_DATA_STATUSf) ==
					YT921X_EDATA_DATA_IDLEv,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_EDATA_DATAm, &val);
		res2 = yt921x_smi_acquire(priv);
		if (unlikely(res != 0))
			return res;
		if (unlikely(res2 != 0))
			return res2;
	}

	*valp = val;
	return 0;
}

static int __yt921x_edata_read(struct yt921x_priv *priv, u8 addr, u8 *valp)
{
	u32 ctrl = YT921X_EDATA_CTRL_ADDRv(addr) | YT921X_EDATA_CTRL_READv;
	u32 val;
	int res;

	res = __yt921x_edata_out(priv, &val);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_EDATA_CTRLm, ctrl);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_edata_out(priv, &val);
	if (unlikely(res != 0))
		return res;

	*valp = FIELD_GET(YT921X_EDATA_DATA_DATAf, val);
	return 0;
}

/******** internal interface mdio ********/

static int __yt921x_intif_wait(struct yt921x_priv *priv)
{
	u32 val;
	int res;

	res = __yt921x_smi_read(priv, YT921X_INTIF_MDIO_OPm, &val);
	if (unlikely(res != 0))
		return res;
	if ((val & YT921X_IF_MDIO_OP_DOf) != 0) {
		int res2;

		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read, res,
					(val & YT921X_IF_MDIO_OP_DOf) == 0,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_INTIF_MDIO_OPm,
					&val);
		res2 = yt921x_smi_acquire(priv);
		if (unlikely(res != 0))
			return res;
		if (unlikely(res2 != 0))
			return res2;
	}

	return 0;
}

static int
__yt921x_intif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	struct device *dev = priv->dev;

	u32 mask = YT921X_IF_MDIO_CTRL_PORTf | YT921X_IF_MDIO_CTRL_REGf |
		   YT921X_IF_MDIO_CTRL_OPf;
	u32 ctrl = YT921X_IF_MDIO_CTRL_PORTv(port) |
		   YT921X_IF_MDIO_CTRL_REGv(reg) |
		   YT921X_IF_MDIO_CTRL_READv;

	u32 val;
	int res;

	res = __yt921x_intif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_update_bits(priv, YT921X_INTIF_MDIO_CTRLm, mask,
				       ctrl);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_INTIF_MDIO_OPm,
				 YT921X_IF_MDIO_OP_DOf);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_intif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_read(priv, YT921X_INTIF_MDIO_READ_DATAm, &val);
	if (unlikely(res != 0))
		return res;

	if (unlikely((u16)val != val))
		dev_err(dev, "%s: Expected u16, got 0x%08x\n", __func__, val);
	*valp = (u16)val;
	return 0;
}

static int
yt921x_intif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_intif_read(priv, port, reg, valp);
	yt921x_smi_release(priv);
	return res;
}

static int
__yt921x_intif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	u32 mask = YT921X_IF_MDIO_CTRL_PORTf | YT921X_IF_MDIO_CTRL_REGf |
		   YT921X_IF_MDIO_CTRL_OPf;
	u32 ctrl = YT921X_IF_MDIO_CTRL_PORTv(port) |
		   YT921X_IF_MDIO_CTRL_REGv(reg) |
		   YT921X_IF_MDIO_CTRL_WRITEv;
	int res;

	res = __yt921x_intif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_update_bits(priv, YT921X_INTIF_MDIO_CTRLm, mask,
				       ctrl);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_INTIF_MDIO_WRITE_DATAm, val);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_INTIF_MDIO_OPm,
				 YT921X_IF_MDIO_OP_DOf);
	if (unlikely(res != 0))
		return res;
	return __yt921x_intif_wait(priv);
}

static int
yt921x_intif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_intif_write(priv, port, reg, val);
	yt921x_smi_release(priv);
	return res;
}

static int yt921x_mbus_int_read(struct mii_bus *mbus, int port, int reg)
{
	struct yt921x_priv *priv = mbus->priv;
	u16 val;
	int res;

	res = yt921x_intif_read(priv, port, reg, &val);
	if (unlikely(res != 0))
		return res;
	return val;
}

static int
yt921x_mbus_int_write(struct mii_bus *mbus, int port, int reg, u16 data)
{
	struct yt921x_priv *priv = mbus->priv;

	return yt921x_intif_write(priv, port, reg, data);
}

static int
yt921x_mbus_int_init(struct yt921x_priv *priv, struct device_node *mnp)
{
	struct device *dev = priv->dev;
	struct mii_bus *mbus;
	int res;

	mbus = devm_mdiobus_alloc(dev);
	if (unlikely(!mbus))
		return -ENOMEM;

	mbus->name = "YT921x internal MDIO bus";
	snprintf(mbus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	mbus->priv = priv;
	mbus->read = yt921x_mbus_int_read;
	mbus->write = yt921x_mbus_int_write;
	mbus->parent = dev;
	mbus->phy_mask = (u32)~GENMASK(YT921X_PORT_CTRLS_NUM - 1, 0);

	if (!mnp)
		res = devm_mdiobus_register(dev, mbus);
	else
		res = devm_of_mdiobus_register(dev, mbus, mnp);
	if (unlikely(res != 0))
		return res;

	priv->mbus_int = mbus;

	return 0;
}

/******** external interface mdio ********/

static int __yt921x_extif_wait(struct yt921x_priv *priv)
{
	u32 val;
	int res;

	res = __yt921x_smi_read(priv, YT921X_EXTIF_MDIO_OPm, &val);
	if (unlikely(res != 0))
		return res;
	if ((val & YT921X_IF_MDIO_OP_DOf) != 0) {
		int res2;

		yt921x_smi_release(priv);
		res = read_poll_timeout(yt921x_smi_read, res,
					(val & YT921X_IF_MDIO_OP_DOf) == 0,
					YT921X_MDIO_SLEEP_US,
					YT921X_MDIO_TIMEOUT_US,
					true, priv, YT921X_EXTIF_MDIO_OPm,
					&val);
		res2 = yt921x_smi_acquire(priv);
		if (unlikely(res != 0))
			return res;
		if (unlikely(res2 != 0))
			return res2;
	}

	return 0;
}

static int
__yt921x_extif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	struct device *dev = priv->dev;

	u32 mask = YT921X_IF_MDIO_CTRL_PORTf | YT921X_IF_MDIO_CTRL_REGf |
		   YT921X_IF_MDIO_CTRL_TYPEf | YT921X_IF_MDIO_CTRL_OPf;
	u32 ctrl = YT921X_IF_MDIO_CTRL_PORTv(port) |
		   YT921X_IF_MDIO_CTRL_REGv(reg) |
		   YT921X_IF_MDIO_CTRL_TYPE_C22v | YT921X_IF_MDIO_CTRL_READv;

	u32 val;
	int res;

	res = __yt921x_extif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_update_bits(priv, YT921X_EXTIF_MDIO_CTRLm, mask,
				       ctrl);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_EXTIF_MDIO_OPm,
				 YT921X_IF_MDIO_OP_DOf);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_extif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_read(priv, YT921X_EXTIF_MDIO_READ_DATAm, &val);
	if (unlikely(res != 0))
		return res;

	if (unlikely((u16)val != val))
		dev_err(dev, "%s: Expected u16, got 0x%08x\n", __func__, val);
	*valp = (u16)val;
	return 0;
}

static int
yt921x_extif_read(struct yt921x_priv *priv, int port, int reg, u16 *valp)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_extif_read(priv, port, reg, valp);
	yt921x_smi_release(priv);
	return res;
}

static int
__yt921x_extif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	u32 mask = YT921X_IF_MDIO_CTRL_PORTf | YT921X_IF_MDIO_CTRL_REGf |
		   YT921X_IF_MDIO_CTRL_TYPEf | YT921X_IF_MDIO_CTRL_OPf;
	u32 ctrl = YT921X_IF_MDIO_CTRL_PORTv(port) |
		   YT921X_IF_MDIO_CTRL_REGv(reg) |
		   YT921X_IF_MDIO_CTRL_TYPE_C22v | YT921X_IF_MDIO_CTRL_WRITEv;
	int res;

	res = __yt921x_extif_wait(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_update_bits(priv, YT921X_EXTIF_MDIO_CTRLm, mask,
				       ctrl);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_EXTIF_MDIO_WRITE_DATAm, val);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_write(priv, YT921X_EXTIF_MDIO_OPm,
				 YT921X_IF_MDIO_OP_DOf);
	if (unlikely(res != 0))
		return res;
	return __yt921x_extif_wait(priv);
}

static int
yt921x_extif_write(struct yt921x_priv *priv, int port, int reg, u16 val)
{
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_extif_write(priv, port, reg, val);
	yt921x_smi_release(priv);
	return res;
}

static int yt921x_mbus_ext_read(struct mii_bus *mbus, int port, int reg)
{
	struct yt921x_priv *priv = mbus->priv;
	u16 val;
	int res;

	res = yt921x_extif_read(priv, port, reg, &val);
	if (unlikely(res != 0))
		return res;
	return val;
}

static int
yt921x_mbus_ext_write(struct mii_bus *mbus, int port, int reg, u16 data)
{
	struct yt921x_priv *priv = mbus->priv;

	return yt921x_extif_write(priv, port, reg, data);
}

static int
yt921x_mbus_ext_init(struct yt921x_priv *priv, struct device_node *mnp)
{
	struct device *dev = priv->dev;
	struct mii_bus *mbus;
	int res;

	mbus = devm_mdiobus_alloc(dev);
	if (unlikely(!mbus))
		return -ENOMEM;

	mbus->name = "YT921x external MDIO bus";
	snprintf(mbus->id, MII_BUS_ID_SIZE, "%s-ext", dev_name(dev));
	mbus->priv = priv;
	mbus->read = yt921x_mbus_ext_read;
	mbus->write = yt921x_mbus_ext_write;
	mbus->parent = dev;
	mbus->phy_mask = (u32)~GENMASK(9, 8);

	if (!mnp)
		res = devm_mdiobus_register(dev, mbus);
	else
		res = devm_of_mdiobus_register(dev, mbus, mnp);
	if (unlikely(res != 0))
		return res;

	priv->mbus_ext = mbus;

	return 0;
}

/******** dsa ********/

static enum dsa_tag_protocol
yt921x_dsa_get_tag_protocol(struct dsa_switch *ds, int port,
			    enum dsa_tag_protocol m)
{
	return DSA_TAG_PROTO_YT921X;
}

static int yt921x_dsa_cpu_port(struct dsa_switch *ds, int *portp)
{
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	struct dsa_port *cpu_dp;
	int port = -1;

	dsa_switch_for_each_cpu_port(cpu_dp, ds) {
		if (unlikely(port >= 0)) {
			dev_warn(dev, "More than one CPU port, %d and %d\n",
				 port, cpu_dp->index);
			continue;
		}
		port = cpu_dp->index;
	}

	if (unlikely(port < 0)) {
		dev_err(dev, "No CPU port\n");
		return -EINVAL;
	}

	*portp = port;
	return 0;
}

static int yt921x_detect(struct yt921x_priv *priv)
{
	struct device *dev = priv->dev;

	const struct yt921x_chip_info *info;
	u32 chipid;
	u32 id;
	u32 mode;
	u8 extmode;

	int res;

	res = yt921x_smi_read(priv, YT921X_CHIP_IDm, &chipid);
	if (unlikely(res != 0))
		return res;

	id = FIELD_GET(YT921X_CHIP_ID_IDf, chipid);

	for (info = yt921x_chip_infos; info->name; info++)
		if (info->id == id)
			goto found_id;

	dev_err(dev, "Unexpected chipid 0x%x\n", chipid);
	return -ENODEV;

found_id:
	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	do {
		res = __yt921x_smi_read(priv, YT921X_CHIP_MODEm, &mode);
		if (unlikely(res != 0))
			break;
		res = __yt921x_edata_read(priv, YT921X_EDATA_EXTMODEm,
					  &extmode);
	} while (0);
	yt921x_smi_release(priv);
	if (unlikely(res != 0))
		return res;

	for (; info->name; info++)
		if (info->id == id && info->mode == mode &&
		    info->extmode == extmode)
			goto found_chip;

	dev_err(dev, "Unexpected chipid 0x%x chipmode 0x%x 0x%x\n",
		chipid, mode, extmode);
	return -ENODEV;

found_chip:
	dev_info(dev,
		 "Motorcomm %s switch, chipid: 0x%x, chipmode: 0x%x 0x%x\n",
		 info->name, chipid, mode, extmode);

	res = yt921x_smi_read(priv, YT921X_PON_STRAP_CAPm,
			      &priv->pon_strap_cap);
	if (unlikely(res != 0))
		return res;

	priv->info = info;
	return 0;
}

static int yt921x_dsa_setup(struct dsa_switch *ds)
{
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	struct device_node *np = dev->of_node;

	struct device_node *child;
	int cpu_port;
	u32 val;
	int res;

	res = yt921x_dsa_cpu_port(ds, &cpu_port);
	if (unlikely(res != 0))
		return res;

	res = yt921x_detect(priv);
	if (unlikely(res != 0))
		return res;

	/* Reset */
	res = yt921x_smi_write(priv, YT921X_RESETm, YT921X_RESET_HWf);
	if (unlikely(res != 0))
		return res;

	/* YT921X_RESET_HWf is almost same as GPIO hard reset. So we need
	 * this delay.
	 */
	usleep_range(10000, 15000);

	res = read_poll_timeout(yt921x_smi_read, res, val == 0,
				YT921X_MDIO_SLEEP_US, YT921X_RESET_TIMEOUT_US,
				false, priv, YT921X_RESETm, &val);
	if (unlikely(res != 0)) {
		dev_err(dev, "Reset timeout\n");
		return res;
	}

	/* Always register one mdio bus for the internal/default mdio bus. This
	 * maybe represented in the device tree, but is optional.
	 */
	child = of_get_child_by_name(np, "mdio");
	res = yt921x_mbus_int_init(priv, child);
	of_node_put(child);
	if (unlikely(res != 0))
		return res;
	ds->user_mii_bus = priv->mbus_int;

	/* Walk the device tree, and see if there are any other nodes which say
	 * they are compatible with the external mdio bus.
	 */
	priv->mbus_ext = NULL;
	for_each_available_child_of_node(np, child) {
		if (!of_device_is_compatible(child,
					     "motorcomm,yt921x-mdio-external"))
			continue;

		res = yt921x_mbus_ext_init(priv, child);
		if (unlikely(res != 0)) {
			of_node_put(child);
			return res;
		}
		break;
	}

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	do {
		/* Enable DSA */
		val = YT921X_EXT_CPU_PORT_TAG_ENf |
		      YT921X_EXT_CPU_PORT_PORT_ENf |
		      YT921X_EXT_CPU_PORT_PORTv(cpu_port);
		res = __yt921x_smi_write(priv, YT921X_EXT_CPU_PORTm, val);
		if (unlikely(res != 0))
			break;

		res = __yt921x_smi_read(priv, YT921X_CPU_TAG_TPIDm, &val);
		if (unlikely(res != 0))
			break;
		priv->tag_eth_p = FIELD_GET(YT921X_CPU_TAG_TPID_TPIDf, val);
		if (priv->tag_eth_p != YT921X_CPU_TAG_TPID_TPIDv) {
			dev_warn(dev, "Tag type 0x%x != 0x%x\n",
				 priv->tag_eth_p, YT921X_CPU_TAG_TPID_TPIDv);
			/* Should we pass tag_eth_p to yt921x_xmit()?
			 * Nah, just enforce a fixed value for now.
			 */
			res = -EINVAL;
			break;
		}

		/* Enable MIB */
		res = __yt921x_smi_set_bits(priv, YT921X_FUNCm,
					    YT921X_FUNC_MIBf);
		if (unlikely(res != 0))
			break;

		val = YT921X_MIB_CTRL_CLEANf | YT921X_MIB_CTRL_ALL_PORTf;
		res = __yt921x_smi_write(priv, YT921X_MIB_CTRLm, val);
	} while (0);
	yt921x_smi_release(priv);
	if (unlikely(res != 0))
		return res;

	return 0;
}

static int yt921x_dsa_get_eeprom_len(struct dsa_switch *ds)
{
	return 0x100;
}

static int
__yt921x_dsa_get_eeprom(struct dsa_switch *ds, struct ethtool_eeprom *eeprom,
			u8 *data)
{
	struct yt921x_priv *priv = ds->priv;
	unsigned int i;
	int res = 0;

	for (i = 0; i < eeprom->len; i++) {
		res = __yt921x_edata_read(priv, eeprom->offset + i, &data[i]);
		if (unlikely(res != 0))
			break;
	}

	eeprom->len = i;
	return res;
}

static int
yt921x_dsa_get_eeprom(struct dsa_switch *ds, struct ethtool_eeprom *eeprom,
		      u8 *data)
{
	struct yt921x_priv *priv = ds->priv;
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_dsa_get_eeprom(ds, eeprom, data);
	yt921x_smi_release(priv);
	return res;
}

static bool yt921x_dsa_support_eee(struct dsa_switch *ds, int port)
{
	struct yt921x_priv *priv = ds->priv;

	return (priv->pon_strap_cap & YT921X_PON_STRAP_EEEf) != 0;
}

static int
__yt921x_dsa_set_mac_eee(struct dsa_switch *ds, int port,
			 struct ethtool_keee *e)
{
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
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

		res = __yt921x_smi_toggle_bits(priv, YT921X_PON_STRAP_FUNCm,
					       YT921X_PON_STRAP_EEEf,
					       !!new_mask);
		if (unlikely(res != 0))
			return res;
		res = __yt921x_smi_toggle_bits(priv, YT921X_PON_STRAP_VALm,
					       YT921X_PON_STRAP_EEEf,
					       !!new_mask);
		if (unlikely(res != 0))
			return res;
	}

	priv->eee_ports_mask = new_mask;

	/* Enable / disable port EEE */
	res = __yt921x_smi_toggle_bits(priv, YT921X_MAC_EEE_CTRLm,
				       YT921X_MAC_EEE_CTRL_ENnf(port), enable);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_smi_toggle_bits(priv, YT921X_MACn_EEE_VALm(port),
				       YT921X_MAC_EEE_VAL_DATAf, enable);
	if (unlikely(res != 0))
		return res;

	return 0;
}

static int
yt921x_dsa_set_mac_eee(struct dsa_switch *ds, int port, struct ethtool_keee *e)
{
	struct yt921x_priv *priv = ds->priv;
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	res = __yt921x_dsa_set_mac_eee(ds, port, e);
	yt921x_smi_release(priv);

	return res;
}

static int
yt921x_dsa_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	/* Could be nop at all, since the default frame size is always set to
	 * maximum after reset, but let's implement it anyway
	 */

	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	int frame_size;

	frame_size = new_mtu + ETH_HLEN + ETH_FCS_LEN;
	if (dsa_is_cpu_port(ds, port))
		frame_size += YT921X_TAG_LEN;

	dev_dbg(dev, "%s: port %d, mtu %d, frame size %d\n", __func__,
		port, new_mtu, frame_size);

	return yt921x_smi_update_bits(priv, YT921X_MACn_JUMBOm(port),
				      YT921X_MAC_FRAME_SIZEf,
				      YT921X_MAC_FRAME_SIZEv(frame_size));
}

static int yt921x_dsa_port_max_mtu(struct dsa_switch *ds, int port)
{
	/* Don't want to brother dsa_is_cpu_port() here, so use a fixed value */
	return YT921X_FRAME_SIZE_MAX - ETH_HLEN - ETH_FCS_LEN - YT921X_TAG_LEN;
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

static int yt921x_mib_read(struct yt921x_priv *priv, int port, void *data)
{
	struct device *dev = priv->dev;
	u32 *buf = data;
	int res;

	res = yt921x_smi_acquire(priv);
	if (unlikely(res != 0))
		return res;
	for (size_t i = 0; i < sizeof(struct yt921x_mib_raw) / sizeof(u32);
	     i++) {
		res = __yt921x_smi_read(priv, YT921X_MIBn_DATAnm(port, i),
					&buf[i]);
		if (unlikely(res != 0)) {
			dev_err(dev, "Cannot read MIB %d at 0x%zx: %i\n",
				port, sizeof(u32) * i, res);
			break;
		}
	}
	yt921x_smi_release(priv);
	return res;
}

static int yt921x_read_mib(struct yt921x_priv *priv, int port)
{
	struct device *dev = priv->dev;
	int res;

	res = yt921x_mib_read(priv, port, &priv->ports[port].mib);
	if (unlikely(res != 0))
		dev_err(dev, "Cannot read MIB %d: %i\n", port, res);

	return res;
}

static void
yt921x_dsa_get_ethtool_stats(struct dsa_switch *ds, int port, uint64_t *data)
{
	struct yt921x_priv *priv = ds->priv;
	unsigned char *buf = (unsigned char *)&priv->ports[port].mib;

	yt921x_read_mib(priv, port);

	for (size_t i = 0; i < ARRAY_SIZE(yt921x_mib_descs); i++) {
		const struct yt921x_mib_desc *desc = &yt921x_mib_descs[i];
		u32 *valp = (u32 *)(buf + desc->offset);

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
	struct yt921x_priv *priv = ds->priv;
	struct yt921x_mib_raw *mib = &priv->ports[port].mib;

	yt921x_read_mib(priv, port);

	s->rx_length_errors = (u64)mib->rx_undersize_errors +
			      mib->rx_fragment_errors;
	s->rx_over_errors = u64_from_u32(mib->rx_over_errors_hi,
					 mib->rx_over_errors_lo);
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
	s->rx_bytes = u64_from_u32(mib->rx_good_bytes_hi, mib->rx_good_bytes_lo) -
		      ETH_FCS_LEN * s->rx_packets;
	s->tx_bytes = u64_from_u32(mib->tx_good_bytes_hi, mib->tx_good_bytes_lo) -
		      ETH_FCS_LEN * s->tx_packets;
	s->rx_errors = (u64)mib->rx_crc_errors + s->rx_length_errors +
		       s->rx_over_errors;
	s->tx_errors = (u64)mib->tx_undersize_errors + mib->tx_aborted_errors +
		       mib->tx_window_errors;
	s->rx_dropped = mib->rx_dropped;
	/* s->tx_dropped */
	s->multicast = mib->rx_multicast;
	s->collisions = u64_from_u32(mib->tx_collisions_hi,
				     mib->tx_collisions_lo);
}

static void
yt921x_dsa_get_pause_stats(struct dsa_switch *ds, int port,
			   struct ethtool_pause_stats *pause_stats)
{
	struct yt921x_priv *priv = ds->priv;
	struct yt921x_mib_raw *mib = &priv->ports[port].mib;

	yt921x_read_mib(priv, port);

	pause_stats->tx_pause_frames = mib->tx_pause;
	pause_stats->rx_pause_frames = mib->rx_pause;
}

static void
yt921x_dsa_phylink_get_caps(struct dsa_switch *ds, int port,
			    struct phylink_config *config)
{
	struct yt921x_priv *priv = ds->priv;
	const struct yt921x_chip_info *info = priv->info;

	if ((BIT(port) & info->intif_mask) != 0) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		config->mac_capabilities = 0;
	} else if ((BIT(port) & info->extif_mask) != 0) {
		/* SGMII */
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_100BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
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
		config->mac_capabilities = MAC_2500FD;
	} else {
		return;
	}

	config->mac_capabilities |= MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				    MAC_10 | MAC_100 | MAC_1000;
}

static const struct dsa_switch_ops yt921x_dsa_switch_ops = {
	.get_tag_protocol	= yt921x_dsa_get_tag_protocol,
	.setup			= yt921x_dsa_setup,
	.get_eeprom_len		= yt921x_dsa_get_eeprom_len,
	.get_eeprom		= yt921x_dsa_get_eeprom,
	.support_eee		= yt921x_dsa_support_eee,
	.set_mac_eee		= yt921x_dsa_set_mac_eee,
	.port_change_mtu	= yt921x_dsa_port_change_mtu,
	.port_max_mtu		= yt921x_dsa_port_max_mtu,
	.get_strings		= yt921x_dsa_get_strings,
	.get_ethtool_stats	= yt921x_dsa_get_ethtool_stats,
	.get_sset_count		= yt921x_dsa_get_sset_count,
	.get_stats64		= yt921x_dsa_get_stats64,
	.get_pause_stats	= yt921x_dsa_get_pause_stats,
	.phylink_get_caps	= yt921x_dsa_phylink_get_caps,
};

static int
__yt921x_port_config(struct yt921x_priv *priv, int port, unsigned int mode,
		     const struct phylink_link_state *state)
{
	const struct yt921x_chip_info *info = priv->info;
	struct device *dev = priv->dev;
	enum yt921x_speed speed;
	bool duplex_full;
	u32 val;
	u32 mask;
	int res;

	speed = YT921X_SPEED_AN;
	val = YT921X_PORT_LINKf;
	if (mode == MLO_AN_FIXED)
		switch (state->speed) {
		case 10:
			speed = YT921X_SPEED_10;
			val = YT921X_PORT_SPEED_10v;
			break;
		case 100:
			speed = YT921X_SPEED_100;
			val = YT921X_PORT_SPEED_100v;
			break;
		case 1000:
			speed = YT921X_SPEED_1000;
			val = YT921X_PORT_SPEED_1000v;
			break;
		case 2500:
			speed = YT921X_SPEED_2500;
			val = YT921X_PORT_SPEED_2500v;
			break;
		default:
			if (unlikely(state->speed >= 0))
				dev_err(dev, "Unsupported speed %d\n",
					state->speed);
			break;
		}

	if (speed != YT921X_SPEED_AN) {
		switch (state->duplex) {
		case DUPLEX_FULL:
			duplex_full = true;
			val |= YT921X_PORT_DUPLEX_FULLf;
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
		val |= YT921X_PORT_RX_FLOW_CONTROLf;
	if ((state->pause & MLO_PAUSE_TX) != 0)
		val |= YT921X_PORT_TX_FLOW_CONTROLf;
	if ((state->pause & MLO_PAUSE_AN) != 0)
		val |= YT921X_PORT_CTRL_FLOW_CONTROL_ANf;

	res = __yt921x_smi_write(priv, YT921X_PORTn_CTRLm(port), val);
	if (unlikely(res != 0))
		return res;

	switch (state->interface) {
	/* SGMII */
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_100BASEX:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		if (unlikely((BIT(port) & info->extif_mask) == 0)) {
			dev_err(dev, "Wrong mode %d on port %d\n",
				state->interface, port);
			return -EINVAL;
		}

		res = __yt921x_smi_set_bits(priv, YT921X_EXTIF_SERDESm,
					    YT921X_EXTIF_SERDES_EXTIFnf(port));
		if (unlikely(res != 0))
			return res;
		res = __yt921x_smi_clear_bits(priv, YT921X_EXTIF_SELm,
					      YT921X_EXTIF_SEL_EXTIFn_XMIIf(port));
		if (unlikely(res != 0))
			return res;

		switch (state->interface) {
		case PHY_INTERFACE_MODE_SGMII:
			val = YT921X_SGMII_MODE_SGMII_PHYv;
			break;
		case PHY_INTERFACE_MODE_100BASEX:
			val = YT921X_SGMII_MODE_100BASEXv;
			break;
		case PHY_INTERFACE_MODE_1000BASEX:
			val = YT921X_SGMII_MODE_1000BASEXv;
			break;
		case PHY_INTERFACE_MODE_2500BASEX:
			val = YT921X_SGMII_MODE_2500BASEXv;
			break;
		default:
			/* unreachable; suppress compiler warning */
			break;
		}
		mask = YT921X_SGMII_MODEf;

		if (speed != YT921X_SPEED_AN) {
			val |= YT921X_SGMII_LINKf;

			switch (speed) {
			case YT921X_SPEED_10:
				val |= YT921X_SGMII_SPEED_10v;
				break;
			case YT921X_SPEED_100:
				val |= YT921X_SGMII_SPEED_100v;
				break;
			case YT921X_SPEED_1000:
				val |= YT921X_SGMII_SPEED_1000v;
				break;
			case YT921X_SPEED_2500:
				val |= YT921X_SGMII_SPEED_2500v;
				break;
			default:
				/* unreachable; suppress compiler warning */
				break;
			}

			if (duplex_full)
				val |= YT921X_SGMII_DUPLEX_FULLf;

			mask |= YT921X_SGMII_LINKf;
			mask |= YT921X_SGMII_SPEEDf;
			mask |= YT921X_PORT_DUPLEX_FULLf;
		}

		res = __yt921x_smi_update_bits(priv, YT921X_SGMIInm(port),
					       mask, val);
		if (unlikely(res != 0))
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
		if (unlikely((BIT(port) & info->extif_mask) == 0)) {
			dev_err(dev, "Wrong mode %d on port %d\n",
				state->interface, port);
			return -EINVAL;
		}

		res = __yt921x_smi_clear_bits(priv, YT921X_EXTIF_SERDESm,
					      YT921X_EXTIF_SERDES_EXTIFnf(port));
		if (unlikely(res != 0))
			return res;
		res = __yt921x_smi_set_bits(priv, YT921X_EXTIF_SELm,
					    YT921X_EXTIF_SEL_EXTIFn_XMIIf(port));
		if (unlikely(res != 0))
			return res;

		val = YT921X_EXTIF_MODE_PORT_ENf;
		switch (state->interface) {
		case PHY_INTERFACE_MODE_MII:
			val |= YT921X_EXTIF_MODE_MODE_MIIv;
			break;
		case PHY_INTERFACE_MODE_REVMII:
			val |= YT921X_EXTIF_MODE_MODE_REVMIIv;
			break;
		case PHY_INTERFACE_MODE_RMII:
			val |= YT921X_EXTIF_MODE_MODE_RMIIv;
			break;
		case PHY_INTERFACE_MODE_REVRMII:
			val |= YT921X_EXTIF_MODE_MODE_REVRMIIv;
			break;
		case PHY_INTERFACE_MODE_RGMII:
		case PHY_INTERFACE_MODE_RGMII_ID:
		case PHY_INTERFACE_MODE_RGMII_RXID:
		case PHY_INTERFACE_MODE_RGMII_TXID:
			val |= YT921X_EXTIF_MODE_MODE_RGMIIv;
			break;
		default:
			/* unreachable; suppress compiler warning */
			break;
		}
		/* TODO: RGMII delay */
		res = __yt921x_smi_update_bits(priv, YT921X_EXTIFn_MODEm(port),
					       YT921X_EXTIF_MODE_PORT_ENf |
					       YT921X_EXTIF_MODE_MODEf,
					       val);
		if (unlikely(res != 0))
			return res;

		if (speed != YT921X_SPEED_AN) {
			val = YT921X_MDIO_POLLING_LINKf;

			switch (speed) {
			case YT921X_SPEED_10:
				val |= YT921X_MDIO_POLLING_SPEED_10v;
				break;
			case YT921X_SPEED_100:
				val |= YT921X_MDIO_POLLING_SPEED_100v;
				break;
			case YT921X_SPEED_1000:
				val |= YT921X_MDIO_POLLING_SPEED_1000v;
				break;
			case YT921X_SPEED_2500:
				val |= YT921X_MDIO_POLLING_SPEED_2500v;
				break;
			default:
				/* unreachable; suppress compiler warning */
				break;
			}
			if (duplex_full)
				val |= YT921X_MDIO_POLLING_DUPLEX_FULLf;

			res = __yt921x_smi_write(priv,
						 YT921X_MDIO_POLLINGnm(port),
						 val);
			if (unlikely(res != 0))
				return res;
		}

		break;
	case PHY_INTERFACE_MODE_INTERNAL:
		break;
	default:
		dev_err(dev, "Wrong mode %d on port %d\n",
			state->interface, port);
		break;
	}

	return 0;
}

static void
yt921x_phylink_mac_config(struct phylink_config *config, unsigned int mode,
			  const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	int port = dp->index;
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	int res;

	dev_dbg(dev,
		"%s: port %d, mode %u, interface %d, speed %d, duplex %d, "
		"pause %d, advertising %lx\n", __func__,
		port, mode, state->interface, state->speed, state->duplex,
		state->pause, *state->advertising);

	do {
		res = yt921x_smi_acquire(priv);
		if (unlikely(res != 0))
			break;
		res = __yt921x_port_config(priv, port, mode, state);
		yt921x_smi_release(priv);
	} while (0);

	if (unlikely(res != 0))
		dev_err(dev, "Cannot configure port %d: %i\n", port, res);
}

static void
yt921x_phylink_mac_link_down(struct phylink_config *config, unsigned int mode,
			     phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	int port = dp->index;
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	u32 val;
	int res;

	dev_dbg(dev, "%s: port %d\n", __func__, port);

	val = YT921X_PORT_RX_MAC_ENf | YT921X_PORT_TX_MAC_ENf;
	res = yt921x_smi_clear_bits(priv, YT921X_PORTn_CTRLm(port), val);
	if (unlikely(res != 0))
		dev_err(dev, "Cannot disable port %d: %i\n", port, res);
}

static void
yt921x_phylink_mac_link_up(struct phylink_config *config,
			   struct phy_device *phydev, unsigned int mode,
			   phy_interface_t interface, int speed, int duplex,
			   bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	int port = dp->index;
	struct dsa_switch *ds = dp->ds;
	struct yt921x_priv *priv = ds->priv;
	struct device *dev = priv->dev;
	u32 val;
	int res;

	dev_dbg(dev,
		"%s: port %d, mode %u, interface %d, speed %d, duplex %d, "
		"tx_pause %d, rx_pause %d\n", __func__, port, mode, interface,
		speed, duplex, tx_pause, rx_pause);

	val = YT921X_PORT_RX_MAC_ENf | YT921X_PORT_TX_MAC_ENf;
	res = yt921x_smi_set_bits(priv, YT921X_PORTn_CTRLm(port), val);
	if (unlikely(res != 0))
		dev_err(dev, "Cannot enable port %d: %i\n", port, res);
}

static const struct phylink_mac_ops yt921x_phylink_mac_ops = {
	.mac_config	= yt921x_phylink_mac_config,
	.mac_link_down	= yt921x_phylink_mac_link_down,
	.mac_link_up	= yt921x_phylink_mac_link_up,
};

/******** device ********/

static int yt921x_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct device_node *np = dev->of_node;
	struct yt921x_priv *priv;
	struct yt921x_smi_mdio *mdio;
	struct dsa_switch *ds;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (unlikely(!priv))
		return -ENOMEM;

	mdio = devm_kzalloc(dev, sizeof(*mdio), GFP_KERNEL);
	if (unlikely(!mdio))
		return -ENOMEM;

	mdio->bus = mdiodev->bus;
	mdio->addr = mdiodev->addr;
	if (of_property_read_u8(np, "switchid", &mdio->switchid) != 0)
		mdio->switchid = 0;

	priv->dev = dev;
	priv->smi_ops = &yt921x_smi_ops_mdio;
	priv->smi_ctx = mdio;

	ds = &priv->ds;
	ds->dev = dev;
	ds->num_ports = YT921X_PORT_CTRLS_NUM;
	ds->priv = priv;
	ds->dev = dev;
	ds->ops = &yt921x_dsa_switch_ops;
	ds->phylink_mac_ops = &yt921x_phylink_mac_ops;

	dev_set_drvdata(dev, ds);

	return dsa_register_switch(ds);
}

static void yt921x_remove(struct mdio_device *mdiodev)
{
	struct dsa_switch *ds = dev_get_drvdata(&mdiodev->dev);

	if (unlikely(!ds))
		return;

	dsa_unregister_switch(ds);
}

static void yt921x_shutdown(struct mdio_device *mdiodev)
{
	struct dsa_switch *ds = dev_get_drvdata(&mdiodev->dev);

	if (unlikely(!ds))
		return;

	dsa_switch_shutdown(ds);
}

static const struct of_device_id yt921x_of_match[] = {
	{ .compatible = "motorcomm,yt9215" },
	{}
};
MODULE_DEVICE_TABLE(of, yt921x_of_match);

static struct mdio_driver yt921x_driver = {
	.probe = yt921x_probe,
	.remove = yt921x_remove,
	.shutdown = yt921x_shutdown,
	.mdiodrv.driver = {
		.name = "yt921x",
		.of_match_table = yt921x_of_match,
	},
};

mdio_module_driver(yt921x_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("Driver for Motorcomm YT921x Switch");
MODULE_LICENSE("GPL");
