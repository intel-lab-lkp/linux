// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 *
 * Copyright (C) 2011-2013 ASIX
 */

#include <linux/module.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/crc32.h>
#include <linux/usb/usbnet.h>
#include <uapi/linux/mdio.h>
#include <linux/mdio.h>
#include <linux/if_vlan.h>

#define AX88179_PHY_ID				0x03
#define AX_EEPROM_LEN				0x100
#define AX88179_EEPROM_MAGIC			0x17900b95
#define AX_MCAST_FLTSIZE			8
#define AX_MAX_MCAST				64
#define AX_INT_PPLS_LINK			((u32)BIT(16))
#define AX_RXHDR_L4_TYPE_MASK			0x1c
#define AX_RXHDR_L4_TYPE_UDP			4
#define AX_RXHDR_L4_TYPE_TCP			16
#define AX_RXHDR_L3CSUM_ERR			2
#define AX_RXHDR_L4CSUM_ERR			1
#define AX_RXHDR_CRC_ERR			((u32)BIT(29))
#define AX_RXHDR_DROP_ERR			((u32)BIT(31))
#define AX_ACCESS_MAC				0x01
#define AX_ACCESS_PHY				0x02
#define AX_ACCESS_EEPROM			0x04
#define AX_ACCESS_EFUS				0x05
#define AX_RELOAD_EEPROM_EFUSE			0x06
#define AX88179A_WAKEUP_SETTING			0x07
#define AX_FW_MODE				0x08
#define AX_GPHY_CTL				0x0F
#define AX88179A_FLASH_READ			0x21
#define AX88179A_FLASH_WRITE			0x24
#define AX88179A_PHY_CLAUSE45			0x27
#define AX88179A_ACCESS_BL			0x2A
#define AX88179A_PHY_POWER			0x31
#define AX88179A_AUTODETACH			0xC0
#define AX_PAUSE_WATERLVL_LOW			0x54
#define AX_PAUSE_WATERLVL_HIGH			0x55

#define AX_FW_MODE_179A				0x01
#define PHYSICAL_LINK_STATUS			0x02
	#define	AX_USB_SS		0x04
	#define	AX_USB_HS		0x02
	#define AX_USB_FS		0x01

#define GENERAL_STATUS				0x03
/* Check AX88179 version. UA1:Bit2 = 0,  UA2:Bit2 = 1 */
	#define	AX_SECLD		0x04

#define AX_CHIP_STATUS				0x05

#define AX_SROM_ADDR				0x07
#define AX_SROM_CMD				0x0a
	#define EEP_RD			0x04
	#define EEP_BUSY		0x10

#define AX_SROM_DATA_LOW			0x08
#define AX_SROM_DATA_HIGH			0x09

#define AX_RX_CTL				0x0b
	#define AX_RX_CTL_DROPCRCERR	0x0100
	#define AX_RX_CTL_IPE		0x0200
	#define AX_RX_CTL_START		0x0080
	#define AX_RX_CTL_AP		0x0020
	#define AX_RX_CTL_AM		0x0010
	#define AX_RX_CTL_AB		0x0008
	#define AX_RX_CTL_AMALL		0x0002
	#define AX_RX_CTL_PRO		0x0001
	#define AX_RX_CTL_STOP		0x0000

#define AX88179A_ETH_TX_GAP			0x0D

#define AX88179A_BFM_DATA			0x0E
	#define AX_TX_QUEUE_CFG		0x02
	#define AX_TX_QUEUE_SET		0x08
	#define AX_TX_Q1_AHB_FC_EN	0x10
	#define AX_TX_Q2_AHB_FC_EN	0x20
	#define AX_XGMII_EN		0x80

#define AX_NODE_ID				0x10
#define AX_MULFLTARY				0x16

#define AX_MEDIUM_STATUS_MODE			0x22
	#define AX_MEDIUM_GIGAMODE	0x01
	#define AX_MEDIUM_FULL_DUPLEX	0x02
	#define AX_MEDIUM_EN_125MHZ	0x08
	#define AX_MEDIUM_RXFLOW_CTRLEN	0x10
	#define AX_MEDIUM_TXFLOW_CTRLEN	0x20
	#define AX_MEDIUM_RECEIVE_EN	0x100
	#define AX_MEDIUM_PS		0x200
	#define AX_MEDIUM_JUMBO_EN	0x8040

#define AX_MONITOR_MOD				0x24
	#define AX_MONITOR_MODE_RWLC	0x02
	#define AX_MONITOR_MODE_RWMP	0x04
	#define AX_MONITOR_MODE_PMEPOL	0x20
	#define AX_MONITOR_MODE_PMETYPE	0x40

#define AX_GPIO_CTRL				0x25
	#define AX_GPIO_CTRL_GPIO3EN	0x80
	#define AX_GPIO_CTRL_GPIO2EN	0x40
	#define AX_GPIO_CTRL_GPIO1EN	0x20

#define AX_PHYPWR_RSTCTL			0x26
	#define AX_PHYPWR_RSTCTL_BZ	0x0010
	#define AX_PHYPWR_RSTCTL_IPRL	0x0020
	#define AX_PHYPWR_RSTCTL_AT	0x1000

#define AX_RX_BULKIN_QCTRL			0x2e

#define AX_GPHY_EEE_CTRL			0x01

#define AX_CLK_SELECT				0x33
	#define AX_CLK_SELECT_BCS	0x01
	#define AX_CLK_SELECT_ACS	0x02
	#define AX_CLK_SELECT_ULR	0x08

#define AX_RXCOE_CTL				0x34
	#define AX_RXCOE_IP		0x01
	#define AX_RXCOE_TCP		0x02
	#define AX_RXCOE_UDP		0x04
	#define AX_RXCOE_TCPV6		0x20
	#define AX_RXCOE_UDPV6		0x40

#define AX_TXCOE_CTL				0x35
	#define AX_TXCOE_IP		0x01
	#define AX_TXCOE_TCP		0x02
	#define AX_TXCOE_UDP		0x04
	#define AX_TXCOE_TCPV6		0x20
	#define AX_TXCOE_UDPV6		0x40

#define AX88179A_MAC_BM_INT_MASK		0x41
#define AX88179A_MAC_BM_RX_DMA_CTL		0x43
#define AX88179A_MAC_BM_TX_DMA_CTL		0x46

#define AX88179A_MAC_RX_STATUS_CDC		0x6D
	#define AX_LSOFC_WCNT_7_ACCESS	0x03
	#define AX_GMII_CRC_APPEND	0x10

#define AX_LEDCTRL				0x73
#define AX88179A_MAC_ARC_CTRL			0x9E
#define AX88179A_MAC_SWP_CTRL			0xB1

#define AX88179A_MAC_TX_PAUSE			0xB2

#define AX88179A_MAC_CDC_DELAY_TX		0xB5

#define AX88179A_MAC_PATH			0xB7
	#define AX_MAC_RX_PATH_READY	0x01
	#define AX_MAC_TX_PATH_READY	0x02

#define AX88179A_NEW_PAUSE_CTRL			0xB8
	#define AX_NEW_PAUSE_EN		0x01

#define AX88179A_MAC_BULK_OUT_CTRL		0xB9
	#define AX_MAC_EFF_EN		0x02

#define AX88179A_MAC_RX_DATA_CDC_CNT		0xC0
	#define AX_MAC_LSO_ERR_EN	0x04
	#define AX_MAC_MIQFFCTRL_FORMAT	0x10
	#define AX_MAC_MIQFFCTRL_DROP_CRC 0x20

#define AX88179A_AUTODETACH_DELAY	(5UL << 8)
#define AX88179A_AUTODETACH_EN		1

#define AX88179A_MAC_LSO_ENHANCE_CTRL		0xC3
	#define AX_LSO_ENHANCE_EN	0x01

#define AX88179A_MAC_TX_HDR_CKSUM		0xCC
#define AX88179A_EP5_EHR			0xF9

#define AX_PHY_POWER				0x02

#define EPHY_LOW_POWER_EN			0x01
#define S5_WOL_EN				0x04
#define S5_WOL_LOW_POWER			0x20

#define GMII_PHY_PHYSR				0x11
	#define GMII_PHY_PHYSR_SMASK	0xc000
	#define GMII_PHY_PHYSR_GIGA	0x8000
	#define GMII_PHY_PHYSR_100	0x4000
	#define GMII_PHY_PHYSR_FULL	0x2000
	#define GMII_PHY_PHYSR_LINK	0x400

#define GMII_LED_ACT				0x1a
	#define	GMII_LED_ACTIVE_MASK	0xff8f
	#define	GMII_LED0_ACTIVE	BIT(4)
	#define	GMII_LED1_ACTIVE	BIT(5)
	#define	GMII_LED2_ACTIVE	BIT(6)

#define GMII_LED_LINK				0x1c
	#define	GMII_LED_LINK_MASK	0xf888
	#define	GMII_LED0_LINK_10	BIT(0)
	#define	GMII_LED0_LINK_100	BIT(1)
	#define	GMII_LED0_LINK_1000	BIT(2)
	#define	GMII_LED1_LINK_10	BIT(4)
	#define	GMII_LED1_LINK_100	BIT(5)
	#define	GMII_LED1_LINK_1000	BIT(6)
	#define	GMII_LED2_LINK_10	BIT(8)
	#define	GMII_LED2_LINK_100	BIT(9)
	#define	GMII_LED2_LINK_1000	BIT(10)
	#define	LED0_ACTIVE		BIT(0)
	#define	LED0_LINK_10		BIT(1)
	#define	LED0_LINK_100		BIT(2)
	#define	LED0_LINK_1000		BIT(3)
	#define	LED0_FD			BIT(4)
	#define	LED0_USB3_MASK		0x001f
	#define	LED1_ACTIVE		BIT(5)
	#define	LED1_LINK_10		BIT(6)
	#define	LED1_LINK_100		BIT(7)
	#define	LED1_LINK_1000		BIT(8)
	#define	LED1_FD			BIT(9)
	#define	LED1_USB3_MASK		0x03e0
	#define	LED2_ACTIVE		BIT(10)
	#define	LED2_LINK_1000		BIT(13)
	#define	LED2_LINK_100		BIT(12)
	#define	LED2_LINK_10		BIT(11)
	#define	LED2_FD			BIT(14)
	#define	LED_VALID		BIT(15)
	#define	LED2_USB3_MASK		0x7c00

#define GMII_PHYPAGE				0x1e
#define GMII_PHY_PAGE_SELECT			0x1f
	#define GMII_PHY_PGSEL_EXT	0x0007
	#define GMII_PHY_PGSEL_PAGE0	0x0000
	#define GMII_PHY_PGSEL_PAGE3	0x0003
	#define GMII_PHY_PGSEL_PAGE5	0x0005

/* TX Descriptor */
#define AX179A_TX_DESC_LEN_MASK		0x1FFFFF
#define AX179A_TX_DESC_DROP_PADD	BIT(28)
#define AX179A_TX_DESC_VLAN		BIT(29)
#define AX179A_TX_DESC_MSS_MASK		0x7FFF
#define AX179A_TX_DESC_MSS_SHIFT	0x20
#define AX179A_TX_DESC_VLAN_MASK	0xFFFF
#define AX179A_TX_DESC_VLAN_SHIFT	0x30

/* RX Packet Descriptor */
#define AX179A_RX_PD_L4_ERR		BIT(0)
#define AX179A_RX_PD_L3_ERR		BIT(1)
#define AX179A_RX_PD_L4_TYPE_MASK	0x1C
#define AX179A_RX_PD_L4_UDP		0x04
#define AX179A_RX_PD_L4_TCP		0x10
#define AX179A_RX_PD_L3_TYPE_MASK	0x60
#define AX179A_RX_PD_L3_IP		0x20
#define AX179A_RX_PD_L3_IP6		0x40

#define AX179A_RX_PD_VLAN		BIT(10)
#define AX179A_RX_PD_RX_OK		BIT(11)
#define AX179A_RX_PD_DROP		BIT(31)
#define AX179A_RX_PD_LEN_MASK	0x7FFF0000
#define AX179A_RX_PD_LEN_SHIFT	0x10
#define AX179A_RX_PD_VLAN_SHIFT	0x20

/* RX Descriptor header */
#define AX179A_RX_DH_PKT_CNT_MASK		0x1FFF
#define AX179A_RX_DH_DESC_OFFSET_MASK	0xFFFFE000
#define AX179A_RX_DH_DESC_OFFSET_SHIFT	0x0D

#define AX179A_RX_HW_PAD			0x02

static int ax88179_reset(struct usbnet *dev);

enum ax_ether_link_speed {
	ETHER_LINK_NONE = 0,
	ETHER_LINK_10   = 1,
	ETHER_LINK_100  = 2,
	ETHER_LINK_1000 = 3,
	ETHER_LINK_2500 = 4,
};

enum ax_chip_version {
	AX_VERSION_INVALID		= 0x0,
	AX_VERSION_AX88179		= 0x4,
	AX_VERSION_AX88179A		= 0x6,	/* Also AX88772D */
	AX_VERSION_AX88279		= 0x7,
};

struct ax88179_data {
	u8  eee_enabled;
	u8  eee_active;
	u16 rxctl;
	u8 in_pm;
	u32 wol_supported;
	u32 wolopts;
	u8 disconnecting;
	u8 chip_version;
	u8 fw_version[4];
	u8 is_ax88772d;
	u8 ip_align;
	u8 link;
	u8 speed;
	u8 full_duplex;
	u8 rx_checksum;
	u8 eeprom_read_cmd;
	u16 eeprom_block;
};

struct ax88179_int_data {
	__le32 intdata1;
	__le32 intdata2;
};

struct ax_bulkin_settings {
	unsigned char ctrl, timer_l, timer_h, size, ifg;
};

static const struct ax_bulkin_settings AX88179_BULKIN_SIZE[] =	{
	{7, 0x4f, 0,	0x12, 0xff},
	{7, 0x20, 3,	0x16, 0xff},
	{7, 0xae, 7,	0x18, 0xff},
	{7, 0xcc, 0x4c, 0x18, 8},
};

static const struct ax_bulkin_settings AX88179A_BULKIN_SIZE[] = {
	{5, 0x7B, 0x00,	0x17, 0x0F},	/* 1G, SS */
	{5, 0xC0, 0x02,	0x06, 0x0F},	/* 1G, HS */
	{7, 0xF0, 0x00,	0x0C, 0x0F},	/* 100M, Full, SS */
	{6, 0x00, 0x00,	0x06, 0x0F},	/* 100M, Half, SS */
	{5, 0xC0, 0x04,	0x06, 0x0F},	/* 100M, Full, HS */
	{7, 0xC0, 0x04,	0x06, 0x0F},	/* 100M, Half, HS */
	{7, 0x00, 0x00,	0x03, 0x3F},	/* FS */
};

static const struct ax_bulkin_settings AX88772D_BULKIN_SIZE[] = {
	{0, 0x00, 0x00,	0x00, 0x00},	/* 1G, SS (unused) */
	{0, 0x00, 0x00,	0x00, 0x00},	/* 1G, HS (unused) */
	{0, 0x00, 0x00,	0x00, 0x00},	/* 100M, Full, SS (unused) */
	{0, 0x00, 0x00,	0x00, 0x00},	/* 100M, Half, SS (unused) */
	{5, 0xC0, 0x04,	0x06, 0x0F},	/* 100M, Full, HS */
	{7, 0xC0, 0x04,	0x06, 0x0F},	/* 100M, Half, HS */
	{7, 0x00, 0x00,	0x03, 0x3F},	/* FS */
};

static const struct ax_bulkin_settings AX88279_BULKIN_SIZE[] = {
	{5, 0x10, 0x01,	0x11, 0x0F},	/* 2.5G */
	{7, 0xB3, 0x01,	0x11, 0x0F},	/* 1G, SS */
	{7, 0xC0, 0x02,	0x06, 0x0F},	/* 1G, HS */
	{7, 0x80, 0x01,	0x03, 0x0F},	/* 100M, Full, SS */
	{7, 0x80, 0x01,	0x03, 0x0F},	/* 100M, Half, SS */
	{7, 0x80, 0x01,	0x03, 0x0F},	/* 100M, Full, HS */
	{7, 0x80, 0x01,	0x03, 0x0F},	/* 100M, Half, HS */
	{7, 0x00, 0x00,	0x03, 0x3F},	/* FS */
};

static void ax88179_set_pm_mode(struct usbnet *dev, bool pm_mode)
{
	struct ax88179_data *ax179_data = dev->driver_priv;

	ax179_data->in_pm = pm_mode;
}

static int ax88179_in_pm(struct usbnet *dev)
{
	struct ax88179_data *ax179_data = dev->driver_priv;

	return ax179_data->in_pm;
}

static int __ax88179_read_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			      u16 size, void *data)
{
	int ret;
	int (*fn)(struct usbnet *, u8, u8, u16, u16, void *, u16);
	struct ax88179_data *ax179_data = dev->driver_priv;

	BUG_ON(!dev);

	if (!ax88179_in_pm(dev))
		fn = usbnet_read_cmd;
	else
		fn = usbnet_read_cmd_nopm;

	ret = fn(dev, cmd, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		 value, index, data, size);

	if (unlikely((ret < 0) && !(ret == -ENODEV && ax179_data->disconnecting)))
		netdev_warn(dev->net, "Failed to read reg index 0x%04x: %d\n",
			    index, ret);

	return ret;
}

static int __ax88179_write_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			       u16 size, const void *data)
{
	int ret;
	int (*fn)(struct usbnet *, u8, u8, u16, u16, const void *, u16);
	struct ax88179_data *ax179_data = dev->driver_priv;

	BUG_ON(!dev);

	if (!ax88179_in_pm(dev))
		fn = usbnet_write_cmd;
	else
		fn = usbnet_write_cmd_nopm;

	ret = fn(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		 value, index, data, size);

	if (unlikely((ret < 0) && !(ret == -ENODEV && ax179_data->disconnecting)))
		netdev_warn(dev->net, "Failed to write reg index 0x%04x: %d\n",
			    index, ret);

	return ret;
}

static void ax88179_write_cmd_async(struct usbnet *dev, u8 cmd, u16 value,
				    u16 index, u16 size, void *data)
{
	u16 buf;

	if (2 == size) {
		buf = *((u16 *)data);
		cpu_to_le16s(&buf);
		usbnet_write_cmd_async(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR |
				       USB_RECIP_DEVICE, value, index, &buf,
				       size);
	} else {
		usbnet_write_cmd_async(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR |
				       USB_RECIP_DEVICE, value, index, data,
				       size);
	}
}

static int ax88179_read_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			    u16 size, void *data)
{
	int ret;

	if (2 == size) {
		u16 buf = 0;
		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf);
		le16_to_cpus(&buf);
		*((u16 *)data) = buf;
	} else if (4 == size) {
		u32 buf = 0;
		ret = __ax88179_read_cmd(dev, cmd, value, index, size, &buf);
		le32_to_cpus(&buf);
		*((u32 *)data) = buf;
	} else {
		ret = __ax88179_read_cmd(dev, cmd, value, index, size, data);
	}

	return ret;
}

static int ax88179_write_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			     u16 size, const void *data)
{
	int ret;

	if (2 == size) {
		u16 buf;
		buf = *((u16 *)data);
		cpu_to_le16s(&buf);
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, &buf);
	} else {
		ret = __ax88179_write_cmd(dev, cmd, value, index,
					  size, data);
	}

	return ret;
}

static void ax88179_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88179_int_data *event;
	u32 link;

	if (urb->actual_length < 8)
		return;

	event = urb->transfer_buffer;
	le32_to_cpus((void *)&event->intdata1);

	link = (((__force u32)event->intdata1) & AX_INT_PPLS_LINK) >> 16;

	if (netif_carrier_ok(dev->net) != link) {
		usbnet_link_change(dev, link, 1);
		if (!link)
			netdev_info(dev->net, "ax88179 - Link status is: 0\n");
	}
}

static int ax88179_mdio_read(struct net_device *netdev, int phy_id, int loc)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 res;

	ax88179_read_cmd(dev, AX_ACCESS_PHY, phy_id, (__u16)loc, 2, &res);
	return res;
}

static void ax88179_mdio_write(struct net_device *netdev, int phy_id, int loc,
			       int val)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 res = (u16) val;

	ax88179_write_cmd(dev, AX_ACCESS_PHY, phy_id, (__u16)loc, 2, &res);
}

static inline int ax88179_phy_mmd_indirect(struct usbnet *dev, u16 prtad,
					   u16 devad)
{
	u16 tmp16;
	int ret;

	tmp16 = devad;
	ret = ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				MII_MMD_CTRL, 2, &tmp16);

	tmp16 = prtad;
	ret = ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				MII_MMD_DATA, 2, &tmp16);

	tmp16 = devad | MII_MMD_CTRL_NOINCR;
	ret = ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				MII_MMD_CTRL, 2, &tmp16);

	return ret;
}

static int
ax88179_phy_read_mmd_indirect(struct usbnet *dev, u16 prtad, u16 devad)
{
	int ret;
	u16 tmp16;

	ax88179_phy_mmd_indirect(dev, prtad, devad);

	ret = ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			       MII_MMD_DATA, 2, &tmp16);
	if (ret < 0)
		return ret;

	return tmp16;
}

static int
ax88179_phy_write_mmd_indirect(struct usbnet *dev, u16 prtad, u16 devad,
			       u16 data)
{
	int ret;

	ax88179_phy_mmd_indirect(dev, prtad, devad);

	ret = ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				MII_MMD_DATA, 2, &data);

	if (ret < 0)
		return ret;

	return 0;
}

static int ax_read_mmd(struct usbnet *dev, u16 dev_addr, u16 reg)
{
	struct ax88179_data *priv = dev->driver_priv;
	u16 res;
	int ret;

	if (priv->chip_version >= AX_VERSION_AX88179A) {
		ret = ax88179_read_cmd(dev, AX88179A_PHY_CLAUSE45, dev_addr, reg, 2, &res);
		if (ret < 0)
			return ret;
		return res;
	}

	return ax88179_phy_read_mmd_indirect(dev, reg, dev_addr);
}

static int ax_write_mmd(struct usbnet *dev, u16 dev_addr, u16 reg, u16 data)
{
	struct ax88179_data *priv = dev->driver_priv;

	if (priv->chip_version >= AX_VERSION_AX88179A)
		return ax88179_write_cmd(dev, AX88179A_PHY_CLAUSE45, dev_addr, reg, 2, &data);

	return ax88179_phy_write_mmd_indirect(dev, reg, dev_addr, data);
}

static int ax88179_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax88179_data *priv = dev->driver_priv;
	u16 tmp16;
	u8 tmp8;

	ax88179_set_pm_mode(dev, true);

	usbnet_suspend(intf, message);

	if (priv->wolopts) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MOD,
				 1, 1, &tmp8);
		if (priv->wolopts & WAKE_PHY)
			tmp8 |= AX_MONITOR_MODE_RWLC;
		if (priv->wolopts & WAKE_MAGIC)
			tmp8 |= AX_MONITOR_MODE_RWMP;

		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MOD,
				  1, 1, &tmp8);

		if (priv->chip_version >= AX_VERSION_AX88179A) {
			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &tmp16);
			tmp16 |= AX_MEDIUM_RECEIVE_EN;
			ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &tmp16);
		}

		if (priv->chip_version == AX_VERSION_AX88279)
			ax88179_write_cmd(dev, AX88179A_WAKEUP_SETTING, 8,
					  EPHY_LOW_POWER_EN | S5_WOL_EN
					  | S5_WOL_LOW_POWER | 0x8000, 0, NULL);

	} else if (priv->chip_version == AX_VERSION_AX88279) {
		ax88179_write_cmd(dev, AX88179A_WAKEUP_SETTING, 8, 0x8000, 0, NULL);
	}

	if (priv->chip_version >= AX_VERSION_AX88179A) {
		ax88179_write_cmd(dev, AX88179A_WAKEUP_SETTING, 0, EPHY_LOW_POWER_EN, 0, NULL);
		ax88179_set_pm_mode(dev, false);
		return 0;
	}

	/* Disable RX path */
	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			 2, 2, &tmp16);
	tmp16 &= ~AX_MEDIUM_RECEIVE_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &tmp16);

	/* Force bulk-in zero length */
	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
			2, 2, &tmp16);

	tmp16 |= AX_PHYPWR_RSTCTL_BZ | AX_PHYPWR_RSTCTL_IPRL;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL,
			2, 2, &tmp16);

	/* change clock */
	tmp8 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &tmp8);

	/* Configure RX control register => stop operation */
	tmp16 = AX_RX_CTL_STOP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &tmp16);

	ax88179_set_pm_mode(dev, false);

	return 0;
}

/* This function is used to enable the autodetach function. */
/* This function is determined by offset 0x43 of EEPROM for the AX88179 */
static int ax88179_auto_detach(struct usbnet *dev)
{
	struct ax88179_data *priv = dev->driver_priv;
	u16 tmp16;
	u8 tmp8;

	if (priv->chip_version >= AX_VERSION_AX88179A) {
		tmp16 = AX88179A_AUTODETACH_DELAY;
		ax88179_write_cmd(dev, AX88179A_AUTODETACH, tmp16, 0, 0, NULL);
		return 0;
	}

	if (ax88179_read_cmd(dev, AX_ACCESS_EEPROM, 0x43, 1, 2, &tmp16) < 0)
		return 0;

	if ((tmp16 == 0xFFFF) || (!(tmp16 & 0x0100)))
		return 0;

	/* Enable Auto Detach bit */
	tmp8 = 0;
	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &tmp8);
	tmp8 |= AX_CLK_SELECT_ULR;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &tmp8);

	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &tmp16);
	tmp16 |= AX_PHYPWR_RSTCTL_AT;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &tmp16);

	return 0;
}

static int ax88179_resume(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax88179_data *ax179_data;
	u8 reg8;

	ax179_data = dev->driver_priv;
	ax88179_set_pm_mode(dev, true);

	usbnet_link_change(dev, 0, 0);

	if (ax179_data->chip_version >= AX_VERSION_AX88179A) {
		ax88179_read_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, &reg8);
		if (!(reg8 & AX_PHY_POWER)) {
			reg8 = AX_PHY_POWER;
			ax88179_write_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, &reg8);
			msleep(250);
		}
		ax88179_write_cmd(dev, AX_FW_MODE, AX_FW_MODE_179A, 0, 0, NULL);

		/* Now, that AX_FW_MODE_179A is enabled, the PHY needs a power-cycle.
		 * PHY-power is re-enabled in ax88179_reset()
		 */
		reg8 = 0;
		ax88179_write_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, &reg8);
		msleep(250);
	}

	ax88179_reset(dev);

	ax88179_set_pm_mode(dev, false);

	return usbnet_resume(intf);
}

static void ax88179_disconnect(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax88179_data *ax179_data;

	if (!dev)
		return;

	ax179_data = dev->driver_priv;
	ax179_data->disconnecting = 1;

	usbnet_disconnect(intf);
}

static void
ax88179_get_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *priv = dev->driver_priv;

	wolinfo->supported = priv->wol_supported;
	wolinfo->wolopts = priv->wolopts;
}

static int
ax88179_set_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *priv = dev->driver_priv;

	if (wolinfo->wolopts & ~(priv->wol_supported))
		return -EINVAL;

	priv->wolopts = wolinfo->wolopts;

	return 0;
}

static int ax88179_get_eeprom_len(struct net_device *net)
{
	return AX_EEPROM_LEN;
}

static int
ax88179_get_eeprom(struct net_device *net, struct ethtool_eeprom *eeprom,
		   u8 *data)
{
	struct usbnet *dev = netdev_priv(net);
	u16 *eeprom_buff;
	int first_word, last_word;
	int i, ret;

	if (eeprom->len == 0)
		return -EINVAL;

	eeprom->magic = AX88179_EEPROM_MAGIC;

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;
	eeprom_buff = kmalloc_array(last_word - first_word + 1, sizeof(u16),
				    GFP_KERNEL);
	if (!eeprom_buff)
		return -ENOMEM;

	/* ax88179/178A returns 2 bytes from eeprom on read */
	for (i = first_word; i <= last_word; i++) {
		ret = __ax88179_read_cmd(dev, AX_ACCESS_EEPROM, i, 1, 2,
					 &eeprom_buff[i - first_word]);
		if (ret < 0) {
			kfree(eeprom_buff);
			return -EIO;
		}
	}

	memcpy(data, (u8 *)eeprom_buff + (eeprom->offset & 1), eeprom->len);
	kfree(eeprom_buff);
	return 0;
}

static int
ax88179_set_eeprom(struct net_device *net, struct ethtool_eeprom *eeprom,
		   u8 *data)
{
	struct usbnet *dev = netdev_priv(net);
	u16 *eeprom_buff;
	int first_word;
	int last_word;
	int ret;
	int i;

	netdev_dbg(net, "write EEPROM len %d, offset %d, magic 0x%x\n",
		   eeprom->len, eeprom->offset, eeprom->magic);

	if (eeprom->len == 0)
		return -EINVAL;

	if (eeprom->magic != AX88179_EEPROM_MAGIC)
		return -EINVAL;

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;

	eeprom_buff = kmalloc_array(last_word - first_word + 1, sizeof(u16),
				    GFP_KERNEL);
	if (!eeprom_buff)
		return -ENOMEM;

	/* align data to 16 bit boundaries, read the missing data from
	   the EEPROM */
	if (eeprom->offset & 1) {
		ret = ax88179_read_cmd(dev, AX_ACCESS_EEPROM, first_word, 1, 2,
				       &eeprom_buff[0]);
		if (ret < 0) {
			netdev_err(net, "Failed to read EEPROM at offset 0x%02x.\n", first_word);
			goto free;
		}
	}

	if ((eeprom->offset + eeprom->len) & 1) {
		ret = ax88179_read_cmd(dev, AX_ACCESS_EEPROM, last_word, 1, 2,
				       &eeprom_buff[last_word - first_word]);
		if (ret < 0) {
			netdev_err(net, "Failed to read EEPROM at offset 0x%02x.\n", last_word);
			goto free;
		}
	}

	memcpy((u8 *)eeprom_buff + (eeprom->offset & 1), data, eeprom->len);

	for (i = first_word; i <= last_word; i++) {
		netdev_dbg(net, "write to EEPROM at offset 0x%02x, data 0x%04x\n",
			   i, eeprom_buff[i - first_word]);
		ret = ax88179_write_cmd(dev, AX_ACCESS_EEPROM, i, 1, 2,
					&eeprom_buff[i - first_word]);
		if (ret < 0) {
			netdev_err(net, "Failed to write EEPROM at offset 0x%02x.\n", i);
			goto free;
		}
		msleep(20);
	}

	/* reload EEPROM data */
	ret = ax88179_write_cmd(dev, AX_RELOAD_EEPROM_EFUSE, 0x0000, 0, 0, NULL);
	if (ret < 0) {
		netdev_err(net, "Failed to reload EEPROM data\n");
		goto free;
	}

	ret = 0;
free:
	kfree(eeprom_buff);
	return ret;
}

static int ax88179_get_link_ksettings(struct net_device *net,
				       struct ethtool_link_ksettings *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *data;
	int v;

	data = dev->driver_priv;

	mii_ethtool_get_link_ksettings(&dev->mii, cmd);

	if (data->chip_version >= AX_VERSION_AX88279) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				 cmd->link_modes.supported);

		v = ax88179_mdio_read(dev->net, dev->mii.phy_id, MII_ADVERTISE);
		if (v >= 0 && v & ADVERTISE_RESV)
			linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
					 cmd->link_modes.advertising);

		v = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_10GBT_STAT);
		if (data->speed == ETHER_LINK_2500) {
			cmd->base.speed = SPEED_2500;
			/* MDIO_AN_10GBT_STAT_LP2_5G is broken, but we can deduce that
			 * the link-partner advertised 2500M if remotely AN succceded
			 * for link speed > 1000M and we locally have a link speed of
			 * 2500M
			 */
			if (v >= 0 && v & MDIO_AN_10GBT_STAT_REMOK)
				linkmode_set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
						 cmd->link_modes.lp_advertising);
		}
	}

	return 0;
}

static int ax88179_set_link_ksettings(struct net_device *net,
				      const struct ethtool_link_ksettings *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *data;
	int v;

	data = dev->driver_priv;

	/* mii_ethtool_set_link_ksettings handles unknown bits in MII_ADVERTISE
	 * transparently, so for the 2.5GBit link speed of the AX_VERSION_AX88279
	 * we just set up ADVERTISE_RESV before calling mii_ethtool_set_link_ksettings
	 * at least for speeds < 2500
	 */
	if (data->chip_version == AX_VERSION_AX88279) {
		v = ax88179_mdio_read(net, dev->mii.phy_id, MII_ADVERTISE);
		if (v < 0)
			return v;

		if (linkmode_test_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
				      cmd->link_modes.advertising))
			v |= ADVERTISE_RESV;
		else
			v &= ~ADVERTISE_RESV;
		ax88179_mdio_write(net, dev->mii.phy_id, MII_ADVERTISE, v);
		if (cmd->base.speed == SPEED_2500)
			return mii_nway_restart(&dev->mii);
	}

	return mii_ethtool_set_link_ksettings(&dev->mii, cmd);
}

static int
ax88179_ethtool_get_eee(struct usbnet *dev, struct ethtool_keee *data)
{
	struct ax88179_data *priv = dev->driver_priv;
	int val;

	/* Get Supported EEE */
	val = ax_read_mmd(dev, MDIO_MMD_PCS, MDIO_PCS_EEE_ABLE);
	if (val < 0)
		return val;
	mii_eee_cap1_mod_linkmode_t(data->supported, val);

	/* Get advertisement EEE */
	val = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_ADV);
	if (val < 0)
		return val;
	mii_eee_cap1_mod_linkmode_t(data->advertised, val);

	/* Get LP advertisement EEE */
	val = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_LPABLE);
	if (val < 0)
		return val;
	mii_eee_cap1_mod_linkmode_t(data->lp_advertised, val);
	if (priv->chip_version >= AX_VERSION_AX88279) {
		val = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_LPABLE2);
		if (val < 0)
			return val;
		mii_eee_cap2_mod_linkmode_adv_t(data->lp_advertised, val);
	}

	return 0;
}

static int
ax88179_ethtool_set_eee(struct usbnet *dev, struct ethtool_keee *data)
{
	u16 tmp16 = linkmode_to_mii_eee_cap1_t(data->advertised);

	return ax_write_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_ADV, tmp16);
}

static int ax88179_chk_eee(struct usbnet *dev)
{
	struct ethtool_cmd ecmd = { .cmd = ETHTOOL_GSET };
	struct ax88179_data *priv = dev->driver_priv;

	mii_ethtool_gset(&dev->mii, &ecmd);

	priv->eee_active = 0;
	if ((priv->chip_version < AX_VERSION_AX88279 && (ecmd.duplex & DUPLEX_FULL)) ||
	    (ecmd.speed == SPEED_1000 && (ecmd.duplex & DUPLEX_FULL))) {
		int eee_lp, eee_cap, eee_adv;
		u32 lp, cap, adv, supported = 0;

		eee_cap = ax_read_mmd(dev, MDIO_MMD_PCS, MDIO_PCS_EEE_ABLE);
		if (eee_cap < 0)
			return false;

		cap = mmd_eee_cap_to_ethtool_sup_t(eee_cap);
		if (!cap)
			return false;

		eee_lp = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_LPABLE);
		if (eee_lp < 0)
			return true;

		eee_adv = ax_read_mmd(dev, MDIO_MMD_AN, MDIO_AN_EEE_ADV);
		if (eee_adv < 0)
			return true;

		adv = mmd_eee_adv_to_ethtool_adv_t(eee_adv);
		lp = mmd_eee_adv_to_ethtool_adv_t(eee_lp);
		supported = (ecmd.speed == SPEED_1000) ?
			     SUPPORTED_1000baseT_Full :
			     SUPPORTED_100baseT_Full;

		if (!(lp & adv & supported))
			return true;

		priv->eee_active = 1;
		return true;
	}

	return false;
}

static void ax88179_eee_config(struct usbnet *dev, bool enable)
{
	struct ax88179_data *priv = dev->driver_priv;
	u16 tmp16;

	if (priv->chip_version < AX_VERSION_AX88179A) {
		tmp16 = GMII_PHY_PGSEL_PAGE3;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_PAGE_SELECT, 2, &tmp16);

		tmp16 = enable ? 0x3247 : 0x3246;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  MII_PHYADDR, 2, &tmp16);
		if (enable) {
			tmp16 = GMII_PHY_PGSEL_PAGE5;
			ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
					  GMII_PHY_PAGE_SELECT, 2, &tmp16);

			tmp16 = 0x0680;
			ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
					  MII_BMSR, 2, &tmp16);
		}

		tmp16 = GMII_PHY_PGSEL_PAGE0;
		ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
				  GMII_PHY_PAGE_SELECT, 2, &tmp16);
	} else {
		ax88179_write_cmd(dev, AX_GPHY_CTL, AX_GPHY_EEE_CTRL, enable, 0, NULL);
	}
}

static int ax88179_get_eee(struct net_device *net, struct ethtool_keee *edata)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *priv;

	priv = dev->driver_priv;

	edata->eee_enabled = priv->eee_enabled;
	edata->eee_active = priv->eee_active;

	return ax88179_ethtool_get_eee(dev, edata);
}

static int ax88179_set_eee(struct net_device *net, struct ethtool_keee *edata)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *priv;
	int ret;

	priv = dev->driver_priv;

	priv->eee_enabled = edata->eee_enabled;
	if (!priv->eee_enabled) {
		ax88179_eee_config(dev, false);
	} else {
		priv->eee_enabled = ax88179_chk_eee(dev);
		if (!priv->eee_enabled)
			return -EOPNOTSUPP;

		ax88179_eee_config(dev, true);
	}

	ret = ax88179_ethtool_set_eee(dev, edata);
	if (ret)
		return ret;

	mii_nway_restart(&dev->mii);

	usbnet_link_change(dev, 0, 0);

	return ret;
}

static const struct ethtool_ops ax88179_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= usbnet_get_msglevel,
	.set_msglevel		= usbnet_set_msglevel,
	.get_wol		= ax88179_get_wol,
	.set_wol		= ax88179_set_wol,
	.get_eeprom_len		= ax88179_get_eeprom_len,
	.get_eeprom		= ax88179_get_eeprom,
	.set_eeprom		= ax88179_set_eeprom,
	.get_eee		= ax88179_get_eee,
	.set_eee		= ax88179_set_eee,
	.nway_reset		= usbnet_nway_reset,
	.get_link_ksettings	= ax88179_get_link_ksettings,
	.set_link_ksettings	= ax88179_set_link_ksettings,
	.get_ts_info		= ethtool_op_get_ts_info,
};

static void ax88179_set_multicast(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax88179_data *data = dev->driver_priv;
	u8 *m_filter = ((u8 *)dev->data);

	data->rxctl = (AX_RX_CTL_START | AX_RX_CTL_AB | AX_RX_CTL_IPE);

	if (net->flags & IFF_PROMISC) {
		data->rxctl |= AX_RX_CTL_PRO;
	} else if (net->flags & IFF_ALLMULTI ||
		   netdev_mc_count(net) > AX_MAX_MCAST) {
		data->rxctl |= AX_RX_CTL_AMALL;
	} else if (netdev_mc_empty(net)) {
		/* just broadcast and directed */
	} else {
		/* We use dev->data for our 8 byte filter buffer
		 * to avoid allocating memory that is tricky to free later
		 */
		u32 crc_bits;
		struct netdev_hw_addr *ha;

		memset(m_filter, 0, AX_MCAST_FLTSIZE);

		netdev_for_each_mc_addr(ha, net) {
			crc_bits = ether_crc(ETH_ALEN, ha->addr) >> 26;
			*(m_filter + (crc_bits >> 3)) |= (1 << (crc_bits & 7));
		}

		ax88179_write_cmd_async(dev, AX_ACCESS_MAC, AX_MULFLTARY,
					AX_MCAST_FLTSIZE, AX_MCAST_FLTSIZE,
					m_filter);

		data->rxctl |= AX_RX_CTL_AM;
	}

	ax88179_write_cmd_async(dev, AX_ACCESS_MAC, AX_RX_CTL,
				2, 2, &data->rxctl);
}

static int
ax88179_set_features(struct net_device *net, netdev_features_t features)
{
	u8 tmp;
	struct usbnet *dev = netdev_priv(net);
	netdev_features_t changed = net->features ^ features;

	if (changed & NETIF_F_IP_CSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &tmp);
		tmp ^= AX_TXCOE_TCP | AX_TXCOE_UDP;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &tmp);
	}

	if (changed & NETIF_F_IPV6_CSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &tmp);
		tmp ^= AX_TXCOE_TCPV6 | AX_TXCOE_UDPV6;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, &tmp);
	}

	if (changed & NETIF_F_RXCSUM) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, &tmp);
		tmp ^= AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP |
		       AX_RXCOE_TCPV6 | AX_RXCOE_UDPV6;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, &tmp);
	}

	return 0;
}

static int ax88179_change_mtu(struct net_device *net, int new_mtu)
{
	struct usbnet *dev = netdev_priv(net);
	u16 tmp16;

	WRITE_ONCE(net->mtu, new_mtu);
	dev->hard_mtu = net->mtu + net->hard_header_len;

	if (net->mtu > 1500) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				 2, 2, &tmp16);
		tmp16 |= AX_MEDIUM_JUMBO_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				  2, 2, &tmp16);
	} else {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				 2, 2, &tmp16);
		tmp16 &= ~AX_MEDIUM_JUMBO_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				  2, 2, &tmp16);
	}

	/* max qlen depend on hard_mtu and rx_urb_size */
	usbnet_update_max_qlen(dev);

	return 0;
}

static int ax88179_set_mac_addr(struct net_device *net, void *p)
{
	struct usbnet *dev = netdev_priv(net);
	struct sockaddr *addr = p;
	int ret;

	if (netif_running(net))
		return -EBUSY;
	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(net, addr->sa_data);

	/* Set the MAC address */
	ret = ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN,
				 ETH_ALEN, net->dev_addr);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct net_device_ops ax88179_netdev_ops = {
	.ndo_open		= usbnet_open,
	.ndo_stop		= usbnet_stop,
	.ndo_start_xmit		= usbnet_start_xmit,
	.ndo_tx_timeout		= usbnet_tx_timeout,
	.ndo_get_stats64	= dev_get_tstats64,
	.ndo_change_mtu		= ax88179_change_mtu,
	.ndo_set_mac_address	= ax88179_set_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= usbnet_mii_ioctl,
	.ndo_set_rx_mode	= ax88179_set_multicast,
	.ndo_set_features	= ax88179_set_features,
};

static int ax88179_check_eeprom(struct usbnet *dev)
{
	u8 i, buf, eeprom[20];
	u16 csum, delay = HZ / 10;
	unsigned long jtimeout;

	/* Read EEPROM content */
	for (i = 0; i < 6; i++) {
		buf = i;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_ADDR,
				      1, 1, &buf) < 0)
			return -EINVAL;

		buf = EEP_RD;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
				      1, 1, &buf) < 0)
			return -EINVAL;

		jtimeout = jiffies + delay;
		do {
			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
					 1, 1, &buf);

			if (time_after(jiffies, jtimeout))
				return -EINVAL;

		} while (buf & EEP_BUSY);

		__ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_LOW,
				   2, 2, &eeprom[i * 2]);

		if ((i == 0) && (eeprom[0] == 0xFF))
			return -EINVAL;
	}

	csum = eeprom[6] + eeprom[7] + eeprom[8] + eeprom[9];
	csum = (csum >> 8) + (csum & 0xff);
	if ((csum + eeprom[10]) != 0xff)
		return -EINVAL;

	return 0;
}

static int ax88179_check_efuse(struct usbnet *dev, u16 *ledmode)
{
	u8	i;
	u8	efuse[64];
	u16	csum = 0;

	if (ax88179_read_cmd(dev, AX_ACCESS_EFUS, 0, 64, 64, efuse) < 0)
		return -EINVAL;

	if (*efuse == 0xFF)
		return -EINVAL;

	for (i = 0; i < 64; i++)
		csum = csum + efuse[i];

	while (csum > 255)
		csum = (csum & 0x00FF) + ((csum >> 8) & 0x00FF);

	if (csum != 0xFF)
		return -EINVAL;

	*ledmode = (efuse[51] << 8) | efuse[52];

	return 0;
}

static int ax88179_convert_old_led(struct usbnet *dev, u16 *ledvalue)
{
	u16 led;

	/* Loaded the old eFuse LED Mode */
	if (ax88179_read_cmd(dev, AX_ACCESS_EEPROM, 0x3C, 1, 2, &led) < 0)
		return -EINVAL;

	led >>= 8;
	switch (led) {
	case 0xFF:
		led = LED0_ACTIVE | LED1_LINK_10 | LED1_LINK_100 |
		      LED1_LINK_1000 | LED2_ACTIVE | LED2_LINK_10 |
		      LED2_LINK_100 | LED2_LINK_1000 | LED_VALID;
		break;
	case 0xFE:
		led = LED0_ACTIVE | LED1_LINK_1000 | LED2_LINK_100 | LED_VALID;
		break;
	case 0xFD:
		led = LED0_ACTIVE | LED1_LINK_1000 | LED2_LINK_100 |
		      LED2_LINK_10 | LED_VALID;
		break;
	case 0xFC:
		led = LED0_ACTIVE | LED1_ACTIVE | LED1_LINK_1000 | LED2_ACTIVE |
		      LED2_LINK_100 | LED2_LINK_10 | LED_VALID;
		break;
	default:
		led = LED0_ACTIVE | LED1_LINK_10 | LED1_LINK_100 |
		      LED1_LINK_1000 | LED2_ACTIVE | LED2_LINK_10 |
		      LED2_LINK_100 | LED2_LINK_1000 | LED_VALID;
		break;
	}

	*ledvalue = led;

	return 0;
}

static int ax88179_led_setting(struct usbnet *dev)
{
	u8 ledfd, value = 0;
	u16 tmp, ledact, ledlink, ledvalue = 0, delay = HZ / 10;
	unsigned long jtimeout;

	/* Check AX88179 version. UA1 or UA2*/
	ax88179_read_cmd(dev, AX_ACCESS_MAC, GENERAL_STATUS, 1, 1, &value);

	if (!(value & AX_SECLD)) {	/* UA1 */
		value = AX_GPIO_CTRL_GPIO3EN | AX_GPIO_CTRL_GPIO2EN |
			AX_GPIO_CTRL_GPIO1EN;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_GPIO_CTRL,
				      1, 1, &value) < 0)
			return -EINVAL;
	}

	/* Check EEPROM */
	if (!ax88179_check_eeprom(dev)) {
		value = 0x42;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_ADDR,
				      1, 1, &value) < 0)
			return -EINVAL;

		value = EEP_RD;
		if (ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
				      1, 1, &value) < 0)
			return -EINVAL;

		jtimeout = jiffies + delay;
		do {
			ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_CMD,
					 1, 1, &value);

			if (time_after(jiffies, jtimeout))
				return -EINVAL;

		} while (value & EEP_BUSY);

		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_HIGH,
				 1, 1, &value);
		ledvalue = (value << 8);

		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_SROM_DATA_LOW,
				 1, 1, &value);
		ledvalue |= value;

		/* load internal ROM for defaule setting */
		if ((ledvalue == 0xFFFF) || ((ledvalue & LED_VALID) == 0))
			ax88179_convert_old_led(dev, &ledvalue);

	} else if (!ax88179_check_efuse(dev, &ledvalue)) {
		if ((ledvalue == 0xFFFF) || ((ledvalue & LED_VALID) == 0))
			ax88179_convert_old_led(dev, &ledvalue);
	} else {
		ax88179_convert_old_led(dev, &ledvalue);
	}

	tmp = GMII_PHY_PGSEL_EXT;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHY_PAGE_SELECT, 2, &tmp);

	tmp = 0x2c;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHYPAGE, 2, &tmp);

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			 GMII_LED_ACT, 2, &ledact);

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			 GMII_LED_LINK, 2, &ledlink);

	ledact &= GMII_LED_ACTIVE_MASK;
	ledlink &= GMII_LED_LINK_MASK;

	if (ledvalue & LED0_ACTIVE)
		ledact |= GMII_LED0_ACTIVE;

	if (ledvalue & LED1_ACTIVE)
		ledact |= GMII_LED1_ACTIVE;

	if (ledvalue & LED2_ACTIVE)
		ledact |= GMII_LED2_ACTIVE;

	if (ledvalue & LED0_LINK_10)
		ledlink |= GMII_LED0_LINK_10;

	if (ledvalue & LED1_LINK_10)
		ledlink |= GMII_LED1_LINK_10;

	if (ledvalue & LED2_LINK_10)
		ledlink |= GMII_LED2_LINK_10;

	if (ledvalue & LED0_LINK_100)
		ledlink |= GMII_LED0_LINK_100;

	if (ledvalue & LED1_LINK_100)
		ledlink |= GMII_LED1_LINK_100;

	if (ledvalue & LED2_LINK_100)
		ledlink |= GMII_LED2_LINK_100;

	if (ledvalue & LED0_LINK_1000)
		ledlink |= GMII_LED0_LINK_1000;

	if (ledvalue & LED1_LINK_1000)
		ledlink |= GMII_LED1_LINK_1000;

	if (ledvalue & LED2_LINK_1000)
		ledlink |= GMII_LED2_LINK_1000;

	tmp = ledact;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_LED_ACT, 2, &tmp);

	tmp = ledlink;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_LED_LINK, 2, &tmp);

	tmp = GMII_PHY_PGSEL_PAGE0;
	ax88179_write_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			  GMII_PHY_PAGE_SELECT, 2, &tmp);

	/* LED full duplex setting */
	ledfd = 0;
	if (ledvalue & LED0_FD)
		ledfd |= 0x01;
	else if ((ledvalue & LED0_USB3_MASK) == 0)
		ledfd |= 0x02;

	if (ledvalue & LED1_FD)
		ledfd |= 0x04;
	else if ((ledvalue & LED1_USB3_MASK) == 0)
		ledfd |= 0x08;

	if (ledvalue & LED2_FD)
		ledfd |= 0x10;
	else if ((ledvalue & LED2_USB3_MASK) == 0)
		ledfd |= 0x20;

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_LEDCTRL, 1, 1, &ledfd);

	return 0;
}

static void ax88179_get_mac_addr(struct usbnet *dev)
{
	u8 mac[ETH_ALEN];

	memset(mac, 0, sizeof(mac));

	/* Maybe the boot loader passed the MAC address via device tree */
	if (!eth_platform_get_mac_address(&dev->udev->dev, mac)) {
		netif_dbg(dev, ifup, dev->net,
			  "MAC address read from device tree");
	} else {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN,
				 ETH_ALEN, mac);
		netif_dbg(dev, ifup, dev->net,
			  "MAC address read from ASIX chip");
	}

	if (is_valid_ether_addr(mac)) {
		eth_hw_addr_set(dev->net, mac);
		if (!is_local_ether_addr(mac))
			dev->net->addr_assign_type = NET_ADDR_PERM;
	} else {
		netdev_info(dev->net, "invalid MAC address, using random\n");
	}

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, ETH_ALEN, ETH_ALEN,
			  dev->net->dev_addr);
}

static int ax88179_bind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88179_data *ax179_data;
	int ret;

	ret = usbnet_get_endpoints(dev, intf);
	if (ret < 0)
		return ret;

	ax179_data = kzalloc_obj(*ax179_data);
	if (!ax179_data)
		return -ENOMEM;

	dev->driver_priv = ax179_data;

	ret = ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_CHIP_STATUS,
			       1, 1, &ax179_data->chip_version);
	if (ret < 0)
		goto err_nodev;

	ax179_data->chip_version = (ax179_data->chip_version & 0xf0) >> 4;
	ax179_data->is_ax88772d = 0;
	ax179_data->ip_align = 1;
	ax179_data->eeprom_read_cmd = AX_ACCESS_EEPROM;
	ax179_data->eeprom_block = 2;

	dev->net->netdev_ops = &ax88179_netdev_ops;
	dev->net->ethtool_ops = &ax88179_ethtool_ops;
	dev->net->needed_headroom = 8;
	dev->net->max_mtu = 4088;

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax88179_mdio_read;
	dev->mii.mdio_write = ax88179_mdio_write;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;
	dev->mii.phy_id = 0x03;
	dev->mii.supports_gmii = 1;

	dev->net->features |= NETIF_F_SG | NETIF_F_IP_CSUM |
			      NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM | NETIF_F_TSO;

	dev->net->hw_features |= dev->net->features;

	netif_set_tso_max_size(dev->net, 16384);

	ax88179_reset(dev);

	return 0;

err_nodev:
	kfree(ax179_data);
	ax179_data = NULL;

	return ret;
}

static int ax88179a_bind(struct usbnet *dev, struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct ax88179_data *ax179_data;
	int ret;

	/* Check if vendor configuration */
	if (udev->actconfig->desc.bConfigurationValue != 1) {
		netdev_info(dev->net, "Switching to vendor mode\n");
		usb_driver_set_configuration(udev, 1);
		return -ENODEV;
	}

	ret = usbnet_get_endpoints(dev, intf);
	if (ret < 0)
		return ret;

	ax179_data = kzalloc_obj(*ax179_data);
	if (!ax179_data)
		return -ENOMEM;

	dev->driver_priv = ax179_data;

	ret = ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_CHIP_STATUS,
			       1, 1, &ax179_data->chip_version);
	if (ret < 0)
		goto err_nodev;

	ax179_data->chip_version = (ax179_data->chip_version & 0xf0) >> 4;
	ax179_data->is_ax88772d = 0;
	if (ax179_data->chip_version == AX_VERSION_AX88179A) {
		if (udev->descriptor.bcdDevice == 0x300)
			ax179_data->is_ax88772d = 1;
	}

	for (int i = 0; i < 3; i++) {
		ret = ax88179_read_cmd(dev, AX88179A_ACCESS_BL, (0xFD + i),
				       1, 1, &ax179_data->fw_version[i]);
		if (ret < 0)
			ax179_data->fw_version[i] = 0xff;
	}
	netdev_info(dev->net, "AX88179A/279/772D Chip Version: %x, FW: %d.%d.%d.%d\n",
		    ax179_data->chip_version,
		    ax179_data->fw_version[0], ax179_data->fw_version[1],
		    ax179_data->fw_version[2], ax179_data->fw_version[3]);

	/* The AX88279 requires both the AX_RX_CTL_IPE and AX_RX_CTL_DROPCRCERR
	 * bits set in AX_RX_CTL for creating correct RX-URBs. AX_RX_CTL_DROPCRCERR
	 * is anyway set for all chips, make sure AX_RX_CTL_IPE is set via ip_align.
	 * Also configure eeprom access parameters.
	 */
	if (ax179_data->chip_version == AX_VERSION_AX88279) {
		ax179_data->ip_align = 1;
		ax179_data->eeprom_read_cmd = AX88179A_FLASH_READ;
		ax179_data->eeprom_block = 256;
	} else {
		ax179_data->ip_align = 0;
		ax179_data->eeprom_read_cmd = AX_ACCESS_EFUS;
		ax179_data->eeprom_block = 20;
	}

	dev->net->netdev_ops = &ax88179_netdev_ops;
	dev->net->ethtool_ops = &ax88179_ethtool_ops;
	dev->net->needed_headroom = 8;
	dev->net->needed_tailroom = 8;
	dev->net->min_mtu = ETH_MIN_MTU;
	dev->hard_mtu = 9 * 1024;
	dev->net->max_mtu = dev->hard_mtu - dev->net->hard_header_len;

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax88179_mdio_read;
	dev->mii.mdio_write = ax88179_mdio_write;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;
	dev->mii.phy_id = 0x03;
	if (!ax179_data->is_ax88772d)
		dev->mii.supports_gmii = 1;

	dev->net->features |= NETIF_F_SG | NETIF_F_IP_CSUM |
			      NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM | NETIF_F_TSO |
			      NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX |
			      NETIF_F_HW_VLAN_CTAG_FILTER;

	dev->net->hw_features |= dev->net->features;

	dev->net->vlan_features = NETIF_F_SG | NETIF_F_IP_CSUM |
				  NETIF_F_IPV6_CSUM | NETIF_F_RXCSUM | NETIF_F_TSO;

	netif_set_tso_max_size(dev->net, 16384);

	/* Enable Transmission of Link Speed byte in interrupt URB */
	ax88179_write_cmd(dev, AX_FW_MODE, AX_FW_MODE_179A, 0, 0, NULL);
	ax88179_write_cmd(dev, AX_RELOAD_EEPROM_EFUSE, 0, 0, 0, NULL);

	/* Read MAC address from DTB or ASIX chip */
	ax88179_get_mac_addr(dev);
	memcpy(dev->net->perm_addr, dev->net->dev_addr, ETH_ALEN);

	return 0;

err_nodev:
	kfree(ax179_data);
	ax179_data = NULL;

	return ret;
}

static void ax88179_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	u16 tmp16;

	/* Configure RX control register => stop operation */
	tmp16 = AX_RX_CTL_STOP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &tmp16);

	tmp16 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, &tmp16);

	/* Power down ethernet PHY */
	tmp16 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, &tmp16);

	kfree(ax179_data);
}

static void ax88179a_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	u16 tmp16;
	u8 tmp8;

	/* Configure RX control register => stop operation */
	tmp16 = AX_RX_CTL_STOP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &tmp16);

	tmp8 = 0;
	ax88179_write_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, &tmp8);

	kfree(ax179_data);
}

static void
ax88179_rx_checksum(struct sk_buff *skb, u32 *pkt_hdr)
{
	skb->ip_summed = CHECKSUM_NONE;

	/* checksum error bit is set */
	if ((*pkt_hdr & AX_RXHDR_L3CSUM_ERR) ||
	    (*pkt_hdr & AX_RXHDR_L4CSUM_ERR))
		return;

	/* It must be a TCP or UDP packet with a valid checksum */
	if (((*pkt_hdr & AX_RXHDR_L4_TYPE_MASK) == AX_RXHDR_L4_TYPE_TCP) ||
	    ((*pkt_hdr & AX_RXHDR_L4_TYPE_MASK) == AX_RXHDR_L4_TYPE_UDP))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
}

static void ax88179a_rx_checksum(struct sk_buff *skb, u64 pkt_desc)
{
	u32 pkt_type;

	skb->ip_summed = CHECKSUM_NONE;
	/* checksum error bit is set */
	if (pkt_desc & AX179A_RX_PD_L4_ERR || pkt_desc & AX179A_RX_PD_L3_ERR)
		return;

	pkt_type = pkt_desc & AX179A_RX_PD_L4_TYPE_MASK;
	/* It must be a TCP or UDP packet with a valid checksum */
	if (pkt_type == AX179A_RX_PD_L4_TCP || pkt_type == AX179A_RX_PD_L4_UDP)
		skb->ip_summed = CHECKSUM_UNNECESSARY;
}

static int ax88179_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	struct sk_buff *ax_skb;
	int pkt_cnt;
	u32 rx_hdr;
	u16 hdr_off;
	u32 *pkt_hdr;

	/* At the end of the SKB, there's a header telling us how many packets
	 * are bundled into this buffer and where we can find an array of
	 * per-packet metadata (which contains elements encoded into u16).
	 */

	/* SKB contents for current firmware:
	 *   <packet 1> <padding>
	 *   ...
	 *   <packet N> <padding>
	 *   <per-packet metadata entry 1> <dummy header>
	 *   ...
	 *   <per-packet metadata entry N> <dummy header>
	 *   <padding2> <rx_hdr>
	 *
	 * where:
	 *   <packet N> contains pkt_len bytes:
	 *		2 bytes of IP alignment pseudo header
	 *		packet received
	 *   <per-packet metadata entry N> contains 4 bytes:
	 *		pkt_len and fields AX_RXHDR_*
	 *   <padding>	0-7 bytes to terminate at
	 *		8 bytes boundary (64-bit).
	 *   <padding2> 4 bytes to make rx_hdr terminate at
	 *		8 bytes boundary (64-bit)
	 *   <dummy-header> contains 4 bytes:
	 *		pkt_len=0 and AX_RXHDR_DROP_ERR
	 *   <rx-hdr>	contains 4 bytes:
	 *		pkt_cnt and hdr_off (offset of
	 *		  <per-packet metadata entry 1>)
	 *
	 * pkt_cnt is number of entrys in the per-packet metadata.
	 * In current firmware there is 2 entrys per packet.
	 * The first points to the packet and the
	 *  second is a dummy header.
	 * This was done probably to align fields in 64-bit and
	 *  maintain compatibility with old firmware.
	 * This code assumes that <dummy header> and <padding2> are
	 *  optional.
	 */

	if (skb->len < 4)
		return 0;
	skb_trim(skb, skb->len - 4);
	rx_hdr = get_unaligned_le32(skb_tail_pointer(skb));
	pkt_cnt = (u16)rx_hdr;
	hdr_off = (u16)(rx_hdr >> 16);

	if (pkt_cnt == 0)
		return 0;

	/* Make sure that the bounds of the metadata array are inside the SKB
	 * (and in front of the counter at the end).
	 */
	if (pkt_cnt * 4 + hdr_off > skb->len)
		return 0;
	pkt_hdr = (u32 *)(skb->data + hdr_off);

	/* Packets must not overlap the metadata array */
	skb_trim(skb, hdr_off);

	for (; pkt_cnt > 0; pkt_cnt--, pkt_hdr++) {
		u16 pkt_len_plus_padd;
		u16 pkt_len;

		le32_to_cpus(pkt_hdr);
		pkt_len = (*pkt_hdr >> 16) & 0x1fff;
		pkt_len_plus_padd = (pkt_len + 7) & 0xfff8;

		/* Skip dummy header used for alignment
		 */
		if (pkt_len == 0)
			continue;

		if (pkt_len_plus_padd > skb->len)
			return 0;

		/* Check CRC or runt packet */
		if ((*pkt_hdr & (AX_RXHDR_CRC_ERR | AX_RXHDR_DROP_ERR)) ||
		    pkt_len < 2 + ETH_HLEN) {
			dev->net->stats.rx_errors++;
			skb_pull(skb, pkt_len_plus_padd);
			continue;
		}

		/* last packet */
		if (pkt_len_plus_padd == skb->len) {
			skb_trim(skb, pkt_len);

			/* Skip IP alignment pseudo header */
			skb_pull(skb, 2);

			ax88179_rx_checksum(skb, pkt_hdr);
			return 1;
		}

		ax_skb = netdev_alloc_skb_ip_align(dev->net, pkt_len);
		if (!ax_skb)
			return 0;
		skb_put(ax_skb, pkt_len);
		memcpy(ax_skb->data, skb->data + 2, pkt_len);

		ax88179_rx_checksum(ax_skb, pkt_hdr);
		usbnet_skb_return(dev, ax_skb);

		skb_pull(skb, pkt_len_plus_padd);
	}

	return 0;
}

static int ax88179a_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	struct sk_buff *ax_skb;
	u32 hdr_off, pkt_end;
	u64 *pkt_desc_ptr;
	u16 vlan_tag;
	u16 pkt_cnt;
	u64 rx_hdr;

	/* SKB contents for AX179A-based chips:
	 *   <packet 1>
	 *   ...
	 *   <packet N>
	 *   <per-packet metadata entry 1>
	 *   ...
	 *   <per-packet metadata entry N>
	 *   <rx_hdr>
	 *
	 * where:
	 *   <packet N> contains pkt_len data bytes and padding:
	 *		2 bytes of IP alignment (optional, depends on AX_RX_CTL_IPE flag)
	 *		packet data received
	 *		optional padding to 8-bytes boundary
	 *   <per-packet metadata entry N> contains 8 bytes:
	 *		pkt_len and fields AX_RXHDR_*
	 *   <rx-hdr>	contains 8 bytes:
	 *		pkt_cnt and hdr_off (offset of <per-packet metadata entry 1>)
	 *
	 * pkt_cnt is number of entries in the per-packet metadata array.
	 */

	if (!skb || skb->len < sizeof(rx_hdr))
		goto err;

	/* RX Descriptor Header */
	skb_trim(skb, skb->len - sizeof(rx_hdr));
	rx_hdr = le64_to_cpup((u64 *)skb_tail_pointer(skb));

	/* Check these packets */
	hdr_off = (rx_hdr & AX179A_RX_DH_DESC_OFFSET_MASK) >> AX179A_RX_DH_DESC_OFFSET_SHIFT;
	pkt_cnt = rx_hdr & AX179A_RX_DH_PKT_CNT_MASK;

	/* Consistency check header position */
	if (hdr_off != skb->len - (pkt_cnt * sizeof(rx_hdr)))
		goto err;

	/* Make sure that the bounds of the metadata array are inside the SKB
	 * (and in front of the counter at the end).
	 */
	if (pkt_cnt * 8 + hdr_off > skb->len)
		goto err;

	/* Packets must not overlap the metadata array */
	skb_trim(skb, hdr_off);

	if (!pkt_cnt)
		goto err;

	/* Get the first RX packet descriptor */
	pkt_desc_ptr = (u64 *)(skb->data + hdr_off);

	pkt_end = 0;
	while (pkt_cnt--) {
		u64 pkt_desc = le64_to_cpup(pkt_desc_ptr);
		u32 pkt_len_plus_padd;
		u32 pkt_len;

		pkt_len = (u32)((pkt_desc & AX179A_RX_PD_LEN_MASK) >> AX179A_RX_PD_LEN_SHIFT)
			  - (ax179_data->ip_align ? 2 : 0);
		pkt_len_plus_padd = ((pkt_len + 7 + (ax179_data->ip_align ? 2 : 0)) & 0x7FFF8);

		pkt_end += pkt_len_plus_padd;
		if (pkt_end > hdr_off || (pkt_cnt == 0 && pkt_end != hdr_off))
			goto err;

		if (pkt_desc & AX179A_RX_PD_DROP || !(pkt_desc & AX179A_RX_PD_RX_OK) ||
		    pkt_len > (dev->hard_mtu + AX179A_RX_HW_PAD)) {
			skb_pull(skb, pkt_len_plus_padd);

			/* Next RX Packet Descriptor */
			pkt_desc_ptr++;
			continue;
		}

		ax_skb = netdev_alloc_skb_ip_align(dev->net, pkt_len);
		if (!ax_skb)
			goto err;

		skb_put(ax_skb, pkt_len);
		memcpy(ax_skb->data, skb->data + (ax179_data->ip_align ? AX179A_RX_HW_PAD : 0),
		       pkt_len);

		if (ax179_data->rx_checksum)
			ax88179a_rx_checksum(ax_skb, pkt_desc);

		if (pkt_desc & AX179A_RX_PD_VLAN) {
			vlan_tag = pkt_desc >> AX179A_RX_PD_VLAN_SHIFT;
			__vlan_hwaccel_put_tag(ax_skb, htons(ETH_P_8021Q),
					       vlan_tag & VLAN_VID_MASK);
		}

		usbnet_skb_return(dev, ax_skb);
		skb_pull(skb, pkt_len_plus_padd);

		/* Next RX Packet Header */
		pkt_desc_ptr++;
	}

	return 1;

err:
	return 0;
}

static struct sk_buff *
ax88179_tx_fixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
	u32 tx_hdr1, tx_hdr2;
	int frame_size = dev->maxpacket;
	int headroom;
	void *ptr;

	tx_hdr1 = skb->len;
	tx_hdr2 = skb_shinfo(skb)->gso_size; /* Set TSO mss */
	if (((skb->len + 8) % frame_size) == 0)
		tx_hdr2 |= 0x80008000;	/* Enable padding */

	headroom = skb_headroom(skb) - 8;

	if ((dev->net->features & NETIF_F_SG) && skb_linearize(skb))
		return NULL;

	if ((skb_header_cloned(skb) || headroom < 0) &&
	    pskb_expand_head(skb, headroom < 0 ? 8 : 0, 0, GFP_ATOMIC)) {
		dev_kfree_skb_any(skb);
		return NULL;
	}

	ptr = skb_push(skb, 8);
	put_unaligned_le32(tx_hdr1, ptr);
	put_unaligned_le32(tx_hdr2, ptr + 4);

	usbnet_set_skb_tx_stats(skb, (skb_shinfo(skb)->gso_segs ?: 1), 0);

	return skb;
}

static struct sk_buff *
ax88179a_tx_fixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
	u64 tx_desc = skb->len & AX179A_TX_DESC_LEN_MASK;
	int frame_size = dev->maxpacket;
	struct sk_buff *ax_skb;
	u64 *tx_desc_ptr;
	int padding_size;
	int headroom;
	int tailroom;
	u16 tci = 0;

	/* TSO MSS */
	tx_desc |= ((u64)(skb_shinfo(skb)->gso_size & AX179A_TX_DESC_MSS_MASK)) <<
		   AX179A_TX_DESC_MSS_SHIFT;

	headroom = (skb->len + sizeof(tx_desc)) % 8;
	padding_size = headroom ? 8 - headroom : 0;

	if (((skb->len + sizeof(tx_desc) + padding_size) % frame_size) == 0) {
		padding_size += 8;
		tx_desc |= AX179A_TX_DESC_DROP_PADD;
	}

	if ((dev->net->features & NETIF_F_HW_VLAN_CTAG_TX) && (vlan_get_tag(skb, &tci) >= 0)) {
		tx_desc |= AX179A_TX_DESC_VLAN;
		tx_desc |= ((u64)tci & AX179A_TX_DESC_VLAN_MASK) << AX179A_TX_DESC_VLAN_SHIFT;
	}

	if (!dev->can_dma_sg && (dev->net->features & NETIF_F_SG) && skb_linearize(skb))
		return NULL;

	headroom = skb_headroom(skb);
	tailroom = skb_tailroom(skb);

	if (!(headroom >= sizeof(tx_desc) && tailroom >= padding_size)) {
		ax_skb = skb_copy_expand(skb, sizeof(tx_desc), padding_size, flags);
		dev_kfree_skb_any(skb);
		skb = ax_skb;
		if (!skb)
			return NULL;
	}
	if (padding_size != 0)
		skb_put_zero(skb, padding_size);
	/* Copy TX header */
	tx_desc_ptr = skb_push(skb, sizeof(tx_desc));
	*tx_desc_ptr = cpu_to_le64(tx_desc);

	usbnet_set_skb_tx_stats(skb, 1, 0);

	return skb;
}

static int ax88179_link_reset(struct usbnet *dev)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	u8 tmp[5], link_sts;
	u16 mode, tmp16, delay = HZ / 10;
	u32 tmp32 = 0x40000000;
	unsigned long jtimeout;

	jtimeout = jiffies + delay;
	while (tmp32 & 0x40000000) {
		mode = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &mode);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2,
				  &ax179_data->rxctl);

		/*link up, check the usb device control TX FIFO full or empty*/
		ax88179_read_cmd(dev, 0x81, 0x8c, 0, 4, &tmp32);

		if (time_after(jiffies, jtimeout))
			return 0;
	}

	mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN |
	       AX_MEDIUM_RXFLOW_CTRLEN;

	ax88179_read_cmd(dev, AX_ACCESS_MAC, PHYSICAL_LINK_STATUS,
			 1, 1, &link_sts);

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
			 GMII_PHY_PHYSR, 2, &tmp16);

	if (!(tmp16 & GMII_PHY_PHYSR_LINK)) {
		netdev_info(dev->net, "ax88179 - Link status is: 0\n");
		return 0;
	} else if (GMII_PHY_PHYSR_GIGA == (tmp16 & GMII_PHY_PHYSR_SMASK)) {
		mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
		if (dev->net->mtu > 1500)
			mode |= AX_MEDIUM_JUMBO_EN;

		if (link_sts & AX_USB_SS)
			memcpy(tmp, &AX88179_BULKIN_SIZE[0], 5);
		else if (link_sts & AX_USB_HS)
			memcpy(tmp, &AX88179_BULKIN_SIZE[1], 5);
		else
			memcpy(tmp, &AX88179_BULKIN_SIZE[3], 5);
	} else if (GMII_PHY_PHYSR_100 == (tmp16 & GMII_PHY_PHYSR_SMASK)) {
		mode |= AX_MEDIUM_PS;

		if (link_sts & (AX_USB_SS | AX_USB_HS))
			memcpy(tmp, &AX88179_BULKIN_SIZE[2], 5);
		else
			memcpy(tmp, &AX88179_BULKIN_SIZE[3], 5);
	} else {
		memcpy(tmp, &AX88179_BULKIN_SIZE[3], 5);
	}

	/* RX bulk configuration */
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, tmp);

	dev->rx_urb_size = (1024 * (tmp[3] + 2));

	if (tmp16 & GMII_PHY_PHYSR_FULL)
		mode |= AX_MEDIUM_FULL_DUPLEX;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &mode);

	ax179_data->eee_enabled = ax88179_chk_eee(dev);

	netif_carrier_on(dev->net);

	netdev_info(dev->net, "ax88179 - Link status is: 1\n");

	return 0;
}

static void ax88179a_bulkin_config(struct usbnet *dev, u8 link_sts)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	const struct ax_bulkin_settings *bulkin_data;
	int index = 0;

	switch (ax179_data->speed) {
	case ETHER_LINK_2500:	/* AX88279 only */
		index = 0;
		break;

	case ETHER_LINK_1000:	/* AX88279 & AX88178A */
		if (ax179_data->chip_version == AX_VERSION_AX88279) {
			if (link_sts & AX_USB_SS)
				index = 1;
			else if (link_sts & AX_USB_HS)
				index = 2;
		} else {
			if (link_sts & AX_USB_SS)
				index = 0;
			else if (link_sts & AX_USB_HS)
				index = 1;
		}
		break;

	case ETHER_LINK_100:
		if (ax179_data->chip_version == AX_VERSION_AX88279) {
			if (link_sts & AX_USB_SS)
				index = 3;
			else if (link_sts & AX_USB_HS)
				index = 5;
			if (!ax179_data->full_duplex)
				index++;
		} else {
			/* AX88279A & AX88277D */
			if (link_sts & AX_USB_SS)
				index = 2;
			else if (link_sts & AX_USB_HS)
				index = 4;
			if (!ax179_data->full_duplex)
				index++;
		}
		break;

	case ETHER_LINK_10:
		if (ax179_data->chip_version == AX_VERSION_AX88279)
			index = 7;
		else
			index = 6;
		break;

	default:	/* No link */
		index = 0;
	}

	if (ax179_data->chip_version == AX_VERSION_AX88279 && (link_sts & AX_USB_FS))
		index = 7;

	if (ax179_data->chip_version == AX_VERSION_AX88279) {
		bulkin_data = AX88279_BULKIN_SIZE;
	} else {
		if (ax179_data->is_ax88772d)
			bulkin_data = AX88772D_BULKIN_SIZE;
		else
			bulkin_data = AX88179A_BULKIN_SIZE;
	}

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, &bulkin_data[index]);
}

static int ax88179a_link_reset(struct usbnet *dev)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	u8 tmp8, link_sts, reg8[3];
	u16 tmp16, mode, speed;

	if (!ax179_data->link) {
		netdev_info(dev->net, "ax88179a - Link status is: 0\n");
		return 0;
	}

	/* Stop RX/TX for link configuration */
	tmp16 = AX_RX_CTL_STOP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &tmp16);
	tmp8 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_PATH, 1, 1, &tmp8);

	tmp8 = 0xa5;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_CDC_DELAY_TX, 1, 1, &tmp8);

	tmp16 = 0x0410;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 2, 2, &tmp16);

	tmp8 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_ETH_TX_GAP, 1, 1, &tmp8);

	tmp8 = 0x07;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_EP5_EHR, 1, 1, &tmp8);

	tmp8 = 0x28 | AX_NEW_PAUSE_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_NEW_PAUSE_CTRL, 1, 1, &tmp8);

	mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN;

	/* Link is up, but some older AX88179A FW versions do not send link speed
	 * and duplex status in interrupt URB, so read it via MII
	 */
	if (!ax179_data->speed) {
		struct ethtool_link_ksettings cmd;

		mii_ethtool_get_link_ksettings(&dev->mii, &cmd);
		ax179_data->full_duplex = cmd.base.duplex;
		switch (cmd.base.speed) {
		case SPEED_1000:
			ax179_data->speed = ETHER_LINK_1000;
			break;
		case SPEED_100:
			ax179_data->speed = ETHER_LINK_100;
			break;
		case SPEED_10:
		default:
			ax179_data->speed = ETHER_LINK_10;
			break;
		};
	}

	speed = 0;
	switch (ax179_data->speed) {
	case ETHER_LINK_2500:
		reg8[0] = 0x00;
		reg8[1] = 0xF8;
		reg8[2] = 0x07;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_TX_PAUSE, 3, 3, reg8);

		reg8[0] = 0x78;
		reg8[1] = (AX_LSOFC_WCNT_7_ACCESS << 5);
		reg8[2] = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_STATUS_CDC, 3, 3, reg8);

		reg8[0] = 0x40;
		reg8[1] = AX_MAC_MIQFFCTRL_FORMAT | AX_MAC_MIQFFCTRL_DROP_CRC | AX_MAC_LSO_ERR_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_DATA_CDC_CNT, 2, 2, reg8);

		tmp8 = AX_XGMII_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_BFM_DATA, 1, 1, &tmp8);

		tmp8 = 0x1C | AX_LSO_ENHANCE_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_LSO_ENHANCE_CTRL, 1, 1, &tmp8);

		mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_FULL_DUPLEX;

		speed = 2500;
		break;

	case ETHER_LINK_1000:
		mode |= AX_MEDIUM_GIGAMODE;
		speed = 1000;
		fallthrough;

	case ETHER_LINK_100:
		reg8[0] = 0x78;
		reg8[1] = (AX_LSOFC_WCNT_7_ACCESS << 5) | AX_GMII_CRC_APPEND;
		reg8[2] = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_STATUS_CDC, 3, 3, reg8);

		tmp8 = 0x40;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_DATA_CDC_CNT, 1, 1, &tmp8);

		speed = speed ? speed : 100;
		break;

	case ETHER_LINK_10:
		reg8[0] = 0xFA;
		reg8[1] = (AX_LSOFC_WCNT_7_ACCESS << 5) | AX_GMII_CRC_APPEND;
		reg8[2] = 0xFF;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_STATUS_CDC, 3, 3, reg8);

		tmp8 = 0xFA;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_RX_DATA_CDC_CNT, 1, 1, &tmp8);

		speed = 10;
		break;
	}

	ax88179_read_cmd(dev, AX_ACCESS_MAC, PHYSICAL_LINK_STATUS, 1, 1, &link_sts);
	ax88179a_bulkin_config(dev, link_sts);

	if (ax179_data->chip_version < AX_VERSION_AX88279) {
		tmp8 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_BFM_DATA, 1, 1, &tmp8);
	}

	if (ax179_data->full_duplex)
		mode |= AX_MEDIUM_FULL_DUPLEX;

	if (dev->net->mtu > 1500)
		mode |= AX_MEDIUM_JUMBO_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &mode);

	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &ax179_data->rxctl);

	tmp8 = AX_MAC_RX_PATH_READY | AX_MAC_TX_PATH_READY;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_PATH, 1, 1, &tmp8);

	ax179_data->eee_enabled = ax88179_chk_eee(dev);

	netif_carrier_on(dev->net);

	netdev_info(dev->net, "ax88179a - Link status is: 1, Link speed: %d, Duplex: %d\n",
		    speed, ax179_data->full_duplex);

	return 0;
}

static int ax88179_reset(struct usbnet *dev)
{
	struct ax88179_data *ax179_data = dev->driver_priv;
	struct ethtool_keee eee_data;
	u16 *tmp16;
	u8 buf[5];
	u8 *tmp;

	tmp16 = (u16 *)buf;
	tmp = (u8 *)buf;

	/* Power up ethernet PHY */
	if (ax179_data->chip_version < AX_VERSION_AX88179A) {
		*tmp16 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, tmp16);

		*tmp16 = AX_PHYPWR_RSTCTL_IPRL;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, tmp16);
		msleep(500);

		*tmp = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, tmp);
		msleep(200);
	} else {
		*tmp = AX_PHY_POWER;
		ax88179_write_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, tmp);
		msleep(250);
	}

	if (ax179_data->chip_version == AX_VERSION_AX88279) {
		*tmp16 = ax88179_mdio_read(dev->net, dev->mii.phy_id, MII_ADVERTISE);
		*tmp16 &= ~(ADVERTISE_10FULL | ADVERTISE_10HALF);
		*tmp16 |= ADVERTISE_RESV; /* Advertise 2.5GBit link */
		ax88179_mdio_write(dev->net, dev->mii.phy_id, MII_ADVERTISE, *tmp16);
	}

	/* Ethernet PHY Auto Detach*/
	ax88179_auto_detach(dev);

	if (ax179_data->chip_version >= AX_VERSION_AX88179A) {
		*tmp = AX_MAC_EFF_EN;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_BULK_OUT_CTRL, 1, 1, tmp);

		*tmp16 = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, tmp16);

		*tmp = 0x04;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 1, 1, tmp);
		*tmp = 0x10;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH, 1, 1, tmp);

		*tmp = 0;
		if (dev->net->features & NETIF_F_HW_VLAN_CTAG_FILTER)
			*tmp |= AX_VLAN_CONTROL_VFE;
		if (dev->net->features & NETIF_F_HW_VLAN_CTAG_RX)
			*tmp |= AX_VLAN_CONTROL_VSO;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_VLAN_ID_CONTROL, 1, 1, tmp);

		*tmp = 0xff;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_BM_INT_MASK, 1, 1, tmp);

		*tmp = 0;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_BM_RX_DMA_CTL, 1, 1, tmp);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_BM_TX_DMA_CTL, 1, 1, tmp);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_ARC_CTRL, 1, 1, tmp);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_SWP_CTRL, 1, 1, tmp);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX88179A_MAC_TX_HDR_CKSUM, 1, 1, tmp);
	}

	/* Read MAC address from DTB or asix chip */
	ax88179_get_mac_addr(dev);
	memcpy(dev->net->perm_addr, dev->net->dev_addr, ETH_ALEN);

	/* RX bulk configuration */
	if (ax179_data->chip_version < AX_VERSION_AX88179A) {
		memcpy(tmp, &AX88179_BULKIN_SIZE[0], 5);
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, tmp);
		*tmp = 0x34;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 1, 1, tmp);

		*tmp = 0x52;
		ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH,
				  1, 1, tmp);
		dev->rx_urb_size = 1024 * 20;
	} else {
		/* The Bulk-Register configuration for the AX88179A is done in
		 * ax88179a_link_reset(), once the link is up for a given link and USB-speed.
		 */
		if (ax179_data->is_ax88772d)
			dev->rx_urb_size = 1024 * 24;
		else
			dev->rx_urb_size = 1024 * 48;
	}

	/* Enable checksum offload */
	*tmp = AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP |
	       AX_RXCOE_TCPV6 | AX_RXCOE_UDPV6;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, tmp);
	ax179_data->rx_checksum = 1;

	*tmp = AX_TXCOE_IP | AX_TXCOE_TCP | AX_TXCOE_UDP |
	       AX_TXCOE_TCPV6 | AX_TXCOE_UDPV6;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, tmp);

	/* Configure RX control register => start operation */
	ax179_data->rxctl = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_START |
			    AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB;
	if (ax179_data->ip_align)
		ax179_data->rxctl |= AX_RX_CTL_IPE;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &ax179_data->rxctl);

	if (ax179_data->chip_version < AX_VERSION_AX88179A)
		*tmp = AX_MONITOR_MODE_PMETYPE | AX_MONITOR_MODE_PMEPOL | AX_MONITOR_MODE_RWMP;
	else
		*tmp = AX_MONITOR_MODE_RWMP;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MOD, 1, 1, tmp);

	/* Configure default medium type => giga */
	*tmp16 = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN |
		 AX_MEDIUM_RXFLOW_CTRLEN | AX_MEDIUM_FULL_DUPLEX;
	if (!ax179_data->is_ax88772d)
		*tmp16 |= AX_MEDIUM_GIGAMODE;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, tmp16);

	/* Check if WoL is supported */
	ax179_data->wol_supported = 0;
	if (ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MONITOR_MOD,
			     1, 1, &tmp) > 0)
		ax179_data->wol_supported = WAKE_MAGIC | WAKE_PHY;

	/* For chips starting with AX88179A, LEDS are configured by the adapter
	 * firmware directly from EEPROM/EFUSE values
	 */
	if (ax179_data->chip_version < AX_VERSION_AX88179A)
		ax88179_led_setting(dev);

	ax179_data->eee_enabled = 0;
	ax179_data->eee_active = 0;

	if (ax179_data->chip_version < AX_VERSION_AX88179A) {
		ax88179_eee_config(dev, false);

		ax88179_ethtool_get_eee(dev, &eee_data);
		linkmode_zero(eee_data.advertised);
		ax88179_ethtool_set_eee(dev, &eee_data);
	} else {
		ax88179_eee_config(dev, true);
		ax88179_ethtool_get_eee(dev, &eee_data);
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, eee_data.advertised);
		if (ax179_data->chip_version >= AX_VERSION_AX88279)
			linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, eee_data.advertised);
		ax88179_ethtool_set_eee(dev, &eee_data);
	}

	/* Restart autoneg */
	mii_nway_restart(&dev->mii);

	usbnet_link_change(dev, 0, 0);

	return 0;
}

static int ax88179_net_reset(struct usbnet *dev)
{
	u16 tmp16;

	ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, GMII_PHY_PHYSR,
			 2, &tmp16);
	if (tmp16) {
		ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
				 2, 2, &tmp16);
		if (!(tmp16 & AX_MEDIUM_RECEIVE_EN)) {
			tmp16 |= AX_MEDIUM_RECEIVE_EN;
			ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
					  2, 2, &tmp16);
		}
	} else {
		ax88179_reset(dev);
	}

	return 0;
}

static int ax88179_stop(struct usbnet *dev)
{
	u16 tmp16;

	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			 2, 2, &tmp16);
	tmp16 &= ~AX_MEDIUM_RECEIVE_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE,
			  2, 2, &tmp16);

	return 0;
}

static int ax88179a_stop(struct usbnet *dev)
{
	u16 reg16;
	u8 reg8;

	ax88179_read_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &reg16);
	reg16 &= ~AX_MEDIUM_RECEIVE_EN;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, &reg16);

	reg16 = 0;
	ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, &reg16);

	reg8 = 0;
	ax88179_read_cmd(dev, AX88179A_PHY_POWER, 0, 0, 1, &reg8);

	return 0;
}

static const struct driver_info ax88179_info = {
	.description = "ASIX AX88179 USB 3.0 Gigabit Ethernet",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info ax88178a_info = {
	.description = "ASIX AX88178A USB 2.0 Gigabit Ethernet",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info ax88179a_info = {
	.description = "ASIX AX88179A USB 3.2 Gigabit Ethernet",
	.bind = ax88179a_bind,
	.unbind = ax88179a_unbind,
	.status = ax88179_status,
	.link_reset = ax88179a_link_reset,
	.reset = ax88179_reset,
	.stop = ax88179a_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX | FLAG_MULTI_PACKET | FLAG_AVOID_UNLINK_URBS,
	.rx_fixup = ax88179a_rx_fixup,
	.tx_fixup = ax88179a_tx_fixup,
};

static const struct driver_info ax88772d_info = {
	.description = "ASIX AX88772D/E USB 2.0 Fast Ethernet",
	.bind = ax88179a_bind,
	.unbind = ax88179a_unbind,
	.status = ax88179_status,
	.link_reset = ax88179a_link_reset,
	.reset = ax88179_reset,
	.stop = ax88179a_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX | FLAG_MULTI_PACKET | FLAG_AVOID_UNLINK_URBS,
	.rx_fixup = ax88179a_rx_fixup,
	.tx_fixup = ax88179a_tx_fixup,
};

static const struct driver_info ax88279_info = {
	.description = "ASIX AX88279 USB 3.2 2.5Gigabit Ethernet",
	.bind = ax88179a_bind,
	.unbind = ax88179a_unbind,
	.status = ax88179_status,
	.link_reset = ax88179a_link_reset,
	.reset = ax88179_reset,
	.stop = ax88179a_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX | FLAG_MULTI_PACKET | FLAG_AVOID_UNLINK_URBS,
	.rx_fixup = ax88179a_rx_fixup,
	.tx_fixup = ax88179a_tx_fixup,
};

static const struct driver_info cypress_GX3_info = {
	.description = "Cypress GX3 SuperSpeed to Gigabit Ethernet Controller",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info dlink_dub1312_info = {
	.description = "D-Link DUB-1312 USB 3.0 to Gigabit Ethernet Adapter",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info sitecom_info = {
	.description = "Sitecom USB 3.0 to Gigabit Adapter",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info samsung_info = {
	.description = "Samsung USB Ethernet Adapter",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info lenovo_info = {
	.description = "Lenovo OneLinkDock Gigabit LAN",
	.bind = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset = ax88179_net_reset,
	.stop = ax88179_stop,
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info belkin_info = {
	.description = "Belkin USB Ethernet Adapter",
	.bind	= ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset	= ax88179_net_reset,
	.stop	= ax88179_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info toshiba_info = {
	.description = "Toshiba USB Ethernet Adapter",
	.bind	= ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset	= ax88179_net_reset,
	.stop = ax88179_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info mct_info = {
	.description = "MCT USB 3.0 Gigabit Ethernet Adapter",
	.bind	= ax88179_bind,
	.unbind	= ax88179_unbind,
	.status	= ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset	= ax88179_net_reset,
	.stop	= ax88179_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info at_umc2000_info = {
	.description = "AT-UMC2000 USB 3.0/USB 3.1 Gen 1 to Gigabit Ethernet Adapter",
	.bind   = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset  = ax88179_net_reset,
	.stop   = ax88179_stop,
	.flags  = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info at_umc200_info = {
	.description = "AT-UMC200 USB 3.0/USB 3.1 Gen 1 to Fast Ethernet Adapter",
	.bind   = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset  = ax88179_net_reset,
	.stop   = ax88179_stop,
	.flags  = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct driver_info at_umc2000sp_info = {
	.description = "AT-UMC2000/SP USB 3.0/USB 3.1 Gen 1 to Gigabit Ethernet Adapter",
	.bind   = ax88179_bind,
	.unbind = ax88179_unbind,
	.status = ax88179_status,
	.link_reset = ax88179_link_reset,
	.reset  = ax88179_net_reset,
	.stop   = ax88179_stop,
	.flags  = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = ax88179_rx_fixup,
	.tx_fixup = ax88179_tx_fixup,
};

static const struct usb_device_id products[] = {
{
	/* ASIX AX88179A/B USB 3.2 Gigabit Ethernet */
	USB_DEVICE_VER(0x0b95, 0x1790, 0x0200, 0x0200),
	.driver_info = (unsigned long)&ax88179a_info,
}, {
	/* ASIX AX88772D USB 2.0 100Mbit Ethernet */
	USB_DEVICE_VER(0x0b95, 0x1790, 0x0300, 0x0300),
	.driver_info = (unsigned long)&ax88772d_info,
}, {
	/* ASIX AX88279 USB 3.2 2.5GBit Ethernet */
	USB_DEVICE_VER(0x0b95, 0x1790, 0x0400, 0x0400),
	.driver_info = (unsigned long)&ax88279_info,
}, {
	/* ASIX AX88179 10/100/1000 */
	USB_DEVICE_AND_INTERFACE_INFO(0x0b95, 0x1790, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&ax88179_info,
}, {
	/* ASIX AX88178A 10/100/1000 */
	USB_DEVICE_AND_INTERFACE_INFO(0x0b95, 0x178a, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&ax88178a_info,
}, {
	/* Cypress GX3 SuperSpeed to Gigabit Ethernet Bridge Controller */
	USB_DEVICE_AND_INTERFACE_INFO(0x04b4, 0x3610, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&cypress_GX3_info,
}, {
	/* D-Link DUB-1312 USB 3.0 to Gigabit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x2001, 0x4a00, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&dlink_dub1312_info,
}, {
	/* Sitecom USB 3.0 to Gigabit Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x0df6, 0x0072, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&sitecom_info,
}, {
	/* Samsung USB Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x04e8, 0xa100, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&samsung_info,
}, {
	/* Lenovo OneLinkDock Gigabit LAN */
	USB_DEVICE_AND_INTERFACE_INFO(0x17ef, 0x304b, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&lenovo_info,
}, {
	/* Belkin B2B128 USB 3.0 Hub + Gigabit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x050d, 0x0128, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&belkin_info,
}, {
	/* Toshiba USB 3.0 GBit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x0930, 0x0a13, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&toshiba_info,
}, {
	/* Magic Control Technology U3-A9003 USB 3.0 Gigabit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x0711, 0x0179, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&mct_info,
}, {
	/* Allied Telesis AT-UMC2000 USB 3.0/USB 3.1 Gen 1 to Gigabit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x07c9, 0x000e, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&at_umc2000_info,
}, {
	/* Allied Telesis AT-UMC200 USB 3.0/USB 3.1 Gen 1 to Fast Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x07c9, 0x000f, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&at_umc200_info,
}, {
	/* Allied Telesis AT-UMC2000/SP USB 3.0/USB 3.1 Gen 1 to Gigabit Ethernet Adapter */
	USB_DEVICE_AND_INTERFACE_INFO(0x07c9, 0x0010, 0xff, 0xff, 0),
	.driver_info = (unsigned long)&at_umc2000sp_info,
},
	{ },
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver ax88179_178a_driver = {
	.name =		"ax88179_178a",
	.id_table =	products,
	.probe =	usbnet_probe,
	.suspend =	ax88179_suspend,
	.resume =	ax88179_resume,
	.reset_resume =	ax88179_resume,
	.disconnect =	ax88179_disconnect,
	.supports_autosuspend = 1,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(ax88179_178a_driver);

MODULE_DESCRIPTION("ASIX AX88179/178A based USB 3.0/2.0 Gigabit Ethernet Devices");
MODULE_LICENSE("GPL");
