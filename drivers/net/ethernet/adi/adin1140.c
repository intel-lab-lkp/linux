// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Analog Devices, Inc. ADIN1140 10BASE-T1S MAC-PHY
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/oa_tc6.h>
#include <linux/phy.h>

#define ADIN1140_MMS_REG(m, r)	((((m) & GENMASK(3, 0)) << 16) |	\
				 ((r) & GENMASK(15, 0)))

#define ADIN1140_MACPHY_ID_REG	ADIN1140_MMS_REG(0x0, 0x1)

#define ADIN1140_CONFIG0_REG		0x0004
#define ADIN1140_CONFIG0_TXFCSVE	BIT(14)
#define ADIN1140_CONFIG0_RFA_ZARFE	BIT(12)
#define ADIN1140_CONFIG0_CPS_64		GENMASK(2, 1)

#define ADIN1140_CONFIG2_REG		ADIN1140_MMS_REG(0x0, 0x6)
#define ADIN1140_CONFIG2_FWD_UNK2HOST	BIT(2)

#define ADIN1140_MAC_P1_LOOP_ADDR_REG	ADIN1140_MMS_REG(0x1, 0xC4)

#define ADIN1140_MAC_ADDR_FILT_UPR_REG		ADIN1140_MMS_REG(0x1, 0x50)
#define ADIN1140_MAC_ADDR_FILT_APPLY2PORT1	BIT(30)
#define ADIN1140_MAC_ADDR_FILT_TO_HOST		BIT(16)

#define ADIN1140_MAC_ADDR_FILT_LWR_REG		ADIN1140_MMS_REG(0x1, 0x51)

#define ADIN1140_MAC_ADDR_MASK_UPR_REG	ADIN1140_MMS_REG(0x1, 0x70)
#define ADIN1140_MAC_ADDR_MASK_LWR_REG	ADIN1140_MMS_REG(0x1, 0x71)

#define ADIN1140_MAC_FILT_MC_SLOT	0U
#define ADIN1140_MAC_FILT_BC_SLOT	1U
#define ADIN1140_MAC_FILT_UC_SLOT	2U
#define ADIN1140_MAC_FILT_MAX_SLOT	16U

#define ADIN1140_RX_FRAME_CNT		ADIN1140_MMS_REG(0x1, 0xA1)
#define ADIN1140_RX_BC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xA2)
#define ADIN1140_RX_MC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xA3)
#define ADIN1140_RX_UC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xA4)
#define ADIN1140_RX_CRC_ERR_CNT		ADIN1140_MMS_REG(0x1, 0xA5)
#define ADIN1140_RX_ALIGN_ERR_CNT	ADIN1140_MMS_REG(0x1, 0xA6)
#define ADIN1140_RX_PREAMBLE_ERR_CNT	ADIN1140_MMS_REG(0x1, 0xA7)
#define ADIN1140_RX_SHORT_ERR_CNT	ADIN1140_MMS_REG(0x1, 0xA8)
#define ADIN1140_RX_LONG_ERR_CNT	ADIN1140_MMS_REG(0x1, 0xA9)
#define ADIN1140_RX_PHY_ERR_CNT		ADIN1140_MMS_REG(0x1, 0xAA)
#define ADIN1140_RX_DRP_FULL_CNT	ADIN1140_MMS_REG(0x1, 0xAB)
#define ADIN1140_RX_DRP_FILTER_CNT	ADIN1140_MMS_REG(0x1, 0xAD)
#define ADIN1140_RX_IFG_ERR_CNT		ADIN1140_MMS_REG(0x1, 0xAE)
#define ADIN1140_TX_FRAME_CNT		ADIN1140_MMS_REG(0x1, 0xB1)
#define ADIN1140_TX_BC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xB2)
#define ADIN1140_TX_MC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xB3)
#define ADIN1140_TX_UC_FRAME_CNT	ADIN1140_MMS_REG(0x1, 0xB4)
#define ADIN1140_TX_SINGLE_COL_CNT	ADIN1140_MMS_REG(0x1, 0xB5)
#define ADIN1140_TX_MULTI_COL_CNT	ADIN1140_MMS_REG(0x1, 0xB6)
#define ADIN1140_TX_DEFERRED_CNT	ADIN1140_MMS_REG(0x1, 0xB7)
#define ADIN1140_TX_LATE_COL_CNT	ADIN1140_MMS_REG(0x1, 0xB8)
#define ADIN1140_TX_EXCESS_COL_CNT	ADIN1140_MMS_REG(0x1, 0xB9)
#define ADIN1140_TX_UNDERRUN_CNT	ADIN1140_MMS_REG(0x1, 0xBA)

/* ADIN1140_MAC_FILT_MAX_SLOT - 3 (multicast, broadcast and unicast
 * reserved slots)
 */
#define ADIN1140_MAC_FILT_AVAIL	13U

#define ADIN1140_PHY_CTRL_DEFAULT	0x1000
#define ADIN1140_PHY_STATUS_DEFAULT	0x082D

#define ADIN1140_PHY_C45_PCS_MMS2	2 /* MMD 3 */
#define ADIN1140_PHY_C45_PMA_PMD_MMS3	3 /* MMD 1 */
#define ADIN1140_PHY_C45_VS_PLCA_MMS4	4 /* MMD 31 */

#define ADIN1140_STATS_CNT		23
#define ADIN1140_STATS_CHECK_DELAY	(3 * HZ)

struct adin1140_statistics_reg {
	const char *name;
	u32 addr;
};

struct adin1140_priv {
	struct net_device *netdev;
	struct oa_tc6 *tc6;
	struct mii_bus *mdiobus;
	struct work_struct rx_mode_work;
	struct delayed_work stats_work;
	/* Protect the stats array from concurrent accesses from
	 * adin1140_stats_work, adin1140_ndo_get_stats64
	 * and adin1140_get_ethtool_stats
	 */
	spinlock_t stat_lock;

	u64 stats[ADIN1140_STATS_CNT];
};

enum adin1140_statistics_entry {
	rx_frames,
	rx_broadcast_frames,
	rx_multicast_frames,
	rx_unicast_frames,
	rx_crc_errors,
	rx_align_errors,
	rx_preamble_errors,
	rx_short_frame_errors,
	rx_long_frame_errors,
	rx_phy_errors,
	rx_fifo_full_dropped,
	rx_addr_filter_dropped,
	rx_ifg_errors,
	tx_frames,
	tx_broadcast_frames,
	tx_multicast_frames,
	tx_unicast_frames,
	tx_single_collision,
	tx_multi_collision,
	tx_deferred,
	tx_late_collision,
	tx_excess_collision,
	tx_underrun,
};

static const struct adin1140_statistics_reg adin1140_stats[] = {
	{.name = "rx_frames", .addr = ADIN1140_RX_FRAME_CNT},
	{.name = "rx_broadcast_frames", .addr = ADIN1140_RX_BC_FRAME_CNT},
	{.name = "rx_multicast_frames", .addr = ADIN1140_RX_MC_FRAME_CNT},
	{.name = "rx_unicast_frames", .addr = ADIN1140_RX_UC_FRAME_CNT},
	{.name = "rx_crc_errors", .addr = ADIN1140_RX_CRC_ERR_CNT},
	{.name = "rx_align_errors", .addr = ADIN1140_RX_ALIGN_ERR_CNT},
	{.name = "rx_preamble_errors", .addr = ADIN1140_RX_PREAMBLE_ERR_CNT},
	{.name = "rx_short_frame_errors", .addr = ADIN1140_RX_SHORT_ERR_CNT},
	{.name = "rx_long_frame_errors", .addr = ADIN1140_RX_LONG_ERR_CNT},
	{.name = "rx_phy_errors", .addr = ADIN1140_RX_PHY_ERR_CNT},
	{.name = "rx_fifo_full_dropped", .addr = ADIN1140_RX_DRP_FULL_CNT},
	{.name = "rx_addr_filt_dropped", .addr = ADIN1140_RX_DRP_FILTER_CNT},
	{.name = "rx_ifg_errors", .addr = ADIN1140_RX_IFG_ERR_CNT},
	{.name = "tx_frames", .addr = ADIN1140_TX_FRAME_CNT},
	{.name = "tx_broadcast_frames", .addr = ADIN1140_TX_BC_FRAME_CNT},
	{.name = "tx_multicast_frames", .addr = ADIN1140_TX_MC_FRAME_CNT},
	{.name = "tx_unicast_frames", .addr = ADIN1140_TX_UC_FRAME_CNT},
	{.name = "tx_single_collision", .addr = ADIN1140_TX_SINGLE_COL_CNT},
	{.name = "tx_multi_collision", .addr = ADIN1140_TX_MULTI_COL_CNT},
	{.name = "tx_deferred", .addr = ADIN1140_TX_DEFERRED_CNT},
	{.name = "tx_late_collision", .addr = ADIN1140_TX_LATE_COL_CNT},
	{.name = "tx_excess_collision", .addr = ADIN1140_TX_EXCESS_COL_CNT},
	{.name = "tx_underrun", .addr = ADIN1140_TX_UNDERRUN_CNT},
};

static int adin1140_mac_filter_set(struct adin1140_priv *priv,
				   const u8 *addr, const u8 *mask,
				   u8 slot)
{
	u32 mask_reg;
	u32 val;
	int ret;

	if (slot >= ADIN1140_MAC_FILT_MAX_SLOT)
		return -ENOSPC;

	ret = oa_tc6_write_register(priv->tc6,
				    ADIN1140_MAC_ADDR_FILT_UPR_REG + 2 * slot,
				    get_unaligned_be16(&addr[0]) |
				    ADIN1140_MAC_ADDR_FILT_APPLY2PORT1 |
				    ADIN1140_MAC_ADDR_FILT_TO_HOST);
	if (ret)
		return ret;

	ret = oa_tc6_write_register(priv->tc6,
				    ADIN1140_MAC_ADDR_FILT_LWR_REG + 2 * slot,
				    get_unaligned_be32(&addr[2]));
	if (ret)
		return ret;

	val = get_unaligned_be16(&mask[0]);
	mask_reg = ADIN1140_MAC_ADDR_MASK_UPR_REG + (2 * slot);

	ret = oa_tc6_write_register(priv->tc6, mask_reg, val);
	if (ret)
		return ret;

	val = get_unaligned_be32(&mask[2]);
	mask_reg = ADIN1140_MAC_ADDR_MASK_LWR_REG + (2 * slot);

	return oa_tc6_write_register(priv->tc6, mask_reg, val);
}

static int adin1140_mac_filter_clear(struct adin1140_priv *priv, u8 slot)
{
	u8 mask[ETH_ALEN];
	u8 addr[ETH_ALEN];

	memset(mask, 0xFF, ETH_ALEN);
	memset(addr, 0x0, ETH_ALEN);

	return adin1140_mac_filter_set(priv, addr, mask, slot);
}

static int adin1140_filter_unicast(struct adin1140_priv *priv)
{
	u8 mask[ETH_ALEN];

	memset(mask, 0xFF, ETH_ALEN);

	return adin1140_mac_filter_set(priv, priv->netdev->dev_addr, mask,
				       ADIN1140_MAC_FILT_UC_SLOT);
}

static int adin1140_filter_all_multicast(struct adin1140_priv *priv, bool en)
{
	u8 multicast_addr[ETH_ALEN] = {1, 0, 0, 0, 0, 0};

	if (en)
		return adin1140_mac_filter_set(priv, multicast_addr,
					       multicast_addr,
					       ADIN1140_MAC_FILT_MC_SLOT);

	return adin1140_mac_filter_clear(priv, ADIN1140_MAC_FILT_MC_SLOT);
}

static int adin1140_filter_broadcast(struct adin1140_priv *priv, bool enabled)
{
	u8 mask[ETH_ALEN];

	if (enabled) {
		memset(mask, 0xFF, ETH_ALEN);
		return adin1140_mac_filter_set(priv, mask, mask,
					       ADIN1140_MAC_FILT_BC_SLOT);
	}

	return adin1140_mac_filter_clear(priv, ADIN1140_MAC_FILT_BC_SLOT);
}

static int adin1140_default_filter_config(struct adin1140_priv *priv)
{
	int ret;

	ret = adin1140_filter_broadcast(priv, true);
	if (ret)
		return ret;

	return adin1140_filter_unicast(priv);
}

static int adin1140_promiscuous_mode(struct adin1140_priv *priv, bool enabled)
{
	int ret;
	u32 val;

	ret = oa_tc6_read_register(priv->tc6, ADIN1140_CONFIG2_REG, &val);
	if (ret)
		return ret;

	if (enabled)
		val |= ADIN1140_CONFIG2_FWD_UNK2HOST;
	else
		val &= ~ADIN1140_CONFIG2_FWD_UNK2HOST;

	return oa_tc6_write_register(priv->tc6, ADIN1140_CONFIG2_REG, val);
}

static void adin1140_rx_mode_work(struct work_struct *work)
{
	struct adin1140_priv *priv = container_of(work, struct adin1140_priv,
						  rx_mode_work);
	struct netdev_hw_addr *ha;
	bool all_multi, promisc;
	u8 mask[ETH_ALEN];
	u8 start, end;
	u32 mac_addrs;
	u8 slot, i;
	int ret;

	/* The ADIN1140 has 16 dest MAC address filter slots:
	 * 0 - reserved for all multicast filter.
	 * 1 - reserved for broadcast filter.
	 * 2 - reserved for the device's own unicast MAC.
	 * 3 -> 15 - available for other unicast/multicast filters.
	 */

	mac_addrs = netdev_uc_count(priv->netdev) +
		    netdev_mc_count(priv->netdev);

	if (priv->netdev->flags & IFF_PROMISC) {
		promisc = true;
		all_multi = false;
	} else if (priv->netdev->flags & IFF_ALLMULTI) {
		promisc = false;
		all_multi = true;
	} else if (mac_addrs <= ADIN1140_MAC_FILT_AVAIL) {
		promisc = false;
		all_multi = false;

		slot = ADIN1140_MAC_FILT_UC_SLOT + 1;
		memset(mask, 0xFF, ETH_ALEN);

		netdev_for_each_uc_addr(ha, priv->netdev) {
			ret = adin1140_mac_filter_set(priv, ha->addr, mask,
						      slot);
			if (ret)
				return;

			slot++;
		}

		netdev_for_each_mc_addr(ha, priv->netdev) {
			ret = adin1140_mac_filter_set(priv, ha->addr, mask,
						      slot);
			if (ret)
				return;

			slot++;
		}
	} else {
		/* The filter table is full. Enable promisc mode. */
		promisc = true;
		all_multi = false;

		start = ADIN1140_MAC_FILT_UC_SLOT + 1;
		end = ADIN1140_MAC_FILT_MAX_SLOT;
		for (i = start; i < end; i++) {
			ret = adin1140_mac_filter_clear(priv, i);
			if (ret)
				return;
		}
	}

	ret = adin1140_promiscuous_mode(priv, promisc);
	if (ret)
		return;

	adin1140_filter_all_multicast(priv, all_multi);
}

static void adin1140_rx_mode(struct net_device *netdev)
{
	struct adin1140_priv *priv = netdev_priv(netdev);

	schedule_work(&priv->rx_mode_work);
}

static void adin1140_stats_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	u64 stat_buff[ADIN1140_STATS_CNT] = {};
	struct adin1140_priv *priv;
	u32 reg_val;
	int ret;
	u32 i;

	priv = container_of(dwork, struct adin1140_priv, stats_work);

	for (i = 0; i < ARRAY_SIZE(adin1140_stats); i++) {
		ret = oa_tc6_read_register(priv->tc6, adin1140_stats[i].addr,
					   &reg_val);
		if (ret)
			break;

		stat_buff[i] = reg_val;
	}

	spin_lock(&priv->stat_lock);
	memcpy(&priv->stats, stat_buff, sizeof(priv->stats));
	spin_unlock(&priv->stat_lock);

	schedule_delayed_work(dwork, ADIN1140_STATS_CHECK_DELAY);
}

static int adin1140_configure(struct adin1140_priv *priv)
{
	u32 val;
	int ret;

	ret = oa_tc6_zero_align_receive_frame_enable(priv->tc6);
	if (ret)
		return ret;

	ret = oa_tc6_read_register(priv->tc6, ADIN1140_CONFIG0_REG, &val);
	if (ret)
		return ret;

	/* Zero-Align Receive Frame Enable */
	val |= ADIN1140_CONFIG0_RFA_ZARFE;

	/* Transmit Frame Check Sequence Validation must be disabled
	 * to allow CRC appending by MAC (CONFIG2.CRC_APPEND)
	 */
	val &= ~ADIN1140_CONFIG0_TXFCSVE;
	val |= ADIN1140_CONFIG0_CPS_64;

	ret = oa_tc6_write_register(priv->tc6, ADIN1140_CONFIG0_REG, val);
	if (ret)
		return ret;

	/* Disable MAC loopback */
	ret = oa_tc6_write_register(priv->tc6, ADIN1140_MAC_P1_LOOP_ADDR_REG,
				    0x0);
	if (ret)
		return ret;

	return adin1140_default_filter_config(priv);
}

static int adin1140_open(struct net_device *netdev)
{
	struct adin1140_priv *priv = netdev_priv(netdev);

	schedule_delayed_work(&priv->stats_work, ADIN1140_STATS_CHECK_DELAY);

	phy_start(netdev->phydev);
	netif_start_queue(netdev);

	return 0;
}

static int adin1140_close(struct net_device *netdev)
{
	struct adin1140_priv *priv = netdev_priv(netdev);

	cancel_delayed_work_sync(&priv->stats_work);

	netif_stop_queue(netdev);
	phy_stop(netdev->phydev);

	return 0;
}

static netdev_tx_t adin1140_start_xmit(struct sk_buff *skb,
				       struct net_device *netdev)
{
	struct adin1140_priv *priv = netdev_priv(netdev);

	/* Pad frames to minimum Ethernet frame size (60 bytes without FCS).
	 * The MAC will append the FCS, but we need to ensure the frame is
	 * at least ETH_ZLEN bytes.
	 */
	if (skb_put_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;

	return oa_tc6_start_xmit(priv->tc6, skb);
}

static int adin1140_ioctl(struct net_device *netdev, struct ifreq *rq, int cmd)
{
	if (!netif_running(netdev))
		return -EINVAL;

	return phy_do_ioctl(netdev, rq, cmd);
}

static int adin1140_set_mac_address(struct net_device *netdev, void *addr)
{
	struct adin1140_priv *priv = netdev_priv(netdev);
	struct sockaddr *address = addr;
	u8 mask[ETH_ALEN];
	int ret;

	ret = eth_prepare_mac_addr_change(netdev, addr);
	if (ret < 0)
		return ret;

	if (ether_addr_equal(address->sa_data, netdev->dev_addr))
		return 0;

	memset(mask, 0xFF, ETH_ALEN);
	ret = adin1140_mac_filter_set(priv, address->sa_data, mask,
				      ADIN1140_MAC_FILT_UC_SLOT);
	if (ret)
		return ret;

	eth_commit_mac_addr_change(netdev, addr);

	return 0;
}

static void adin1140_ndo_get_stats64(struct net_device *dev,
				     struct rtnl_link_stats64 *storage)
{
	struct adin1140_priv *priv = netdev_priv(dev);

	storage->rx_packets = priv->netdev->stats.rx_packets;
	storage->tx_packets = priv->netdev->stats.tx_packets;

	storage->rx_bytes = priv->netdev->stats.rx_bytes;
	storage->tx_bytes = priv->netdev->stats.tx_bytes;

	spin_lock(&priv->stat_lock);

	storage->rx_errors = priv->stats[rx_crc_errors] +
			     priv->stats[rx_align_errors] +
			     priv->stats[rx_preamble_errors] +
			     priv->stats[rx_short_frame_errors] +
			     priv->stats[rx_long_frame_errors] +
			     priv->stats[rx_phy_errors] +
			     priv->stats[rx_ifg_errors];

	storage->tx_errors = priv->stats[tx_excess_collision] +
			     priv->stats[tx_underrun];

	storage->rx_dropped = priv->stats[rx_fifo_full_dropped] +
			      priv->stats[rx_addr_filter_dropped];

	storage->multicast = priv->stats[rx_multicast_frames];

	storage->collisions = priv->stats[tx_single_collision] +
			      priv->stats[tx_multi_collision];

	storage->rx_length_errors = priv->stats[rx_short_frame_errors] +
				    priv->stats[rx_long_frame_errors];
	storage->rx_over_errors = priv->stats[rx_fifo_full_dropped];
	storage->rx_crc_errors = priv->stats[rx_crc_errors];
	storage->rx_frame_errors = priv->stats[rx_align_errors];
	storage->rx_missed_errors = priv->stats[rx_fifo_full_dropped];

	storage->tx_aborted_errors = priv->stats[tx_excess_collision];
	storage->tx_fifo_errors = priv->stats[tx_underrun];
	storage->tx_window_errors = priv->stats[tx_late_collision];

	spin_unlock(&priv->stat_lock);
}

static void adin1140_get_drvinfo(struct net_device *netdev,
				 struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "ADIN1140", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(netdev->dev.parent),
		sizeof(info->bus_info));
}

static void adin1140_get_ethtool_stats(struct net_device *netdev,
				       struct ethtool_stats *stats, u64 *data)
{
	struct adin1140_priv *priv = netdev_priv(netdev);

	spin_lock(&priv->stat_lock);
	memcpy(data, &priv->stats, sizeof(u64) * ARRAY_SIZE(adin1140_stats));
	spin_unlock(&priv->stat_lock);
}

static void adin1140_get_ethtool_strings(struct net_device *netdev, u32 sset,
					 u8 *p)
{
	u32 i;

	switch (sset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(adin1140_stats); i++)
			ethtool_puts(&p, adin1140_stats[i].name);

		break;
	}
}

static int adin1140_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(adin1140_stats);
	default:
		return -EOPNOTSUPP;
	}
}

static int adin1140_get_phy_c45_mms(int devnum)
{
	switch (devnum) {
	case MDIO_MMD_PCS:
		return ADIN1140_PHY_C45_PCS_MMS2;
	case MDIO_MMD_PMAPMD:
		return ADIN1140_PHY_C45_PMA_PMD_MMS3;
	case MDIO_MMD_VEND2:
		return ADIN1140_PHY_C45_VS_PLCA_MMS4;
	default:
		return devnum;
	}
}

static int adin1140_mdiobus_read_c45(struct mii_bus *bus, int addr,
				     int devnum, int regnum)
{
	struct oa_tc6 *tc6 = bus->priv;
	u32 regval;
	u32 mms;
	int ret;

	mms = adin1140_get_phy_c45_mms(devnum);
	ret = oa_tc6_read_register(tc6, ADIN1140_MMS_REG(mms, regnum),
				   &regval);
	if (ret)
		return ret;

	return regval;
}

static int adin1140_mdiobus_write_c45(struct mii_bus *bus, int addr,
				      int devnum, int regnum, u16 val)
{
	struct oa_tc6 *tc6 = bus->priv;
	int ret;

	ret = adin1140_get_phy_c45_mms(devnum);
	if (ret < 0)
		return ret;

	return oa_tc6_write_register(tc6, ADIN1140_MMS_REG(ret, regnum), val);
}

static int adin1140_mdiobus_read(struct mii_bus *bus, int addr, int regnum)
{
	struct oa_tc6 *tc6 = bus->priv;
	u32 reg_val;
	int ret;

	/* The ADIN1140's standard PHY C22 register map (OA TC6 0xFF00 -
	 * 0xFF1F), of which only 0xFF00 - 0xFF03 are implemented) cannot be
	 * accessed while frames are being received by the PHY. In case this
	 * happens the CONFIG0 and CONFIG2 register values will get corrupted,
	 * getting a random value. Both reads and writes cause the same
	 * behavior. This is a workaround that avoids MDIO accesses all
	 * together. Since this is a 10BASE-T1S PHY, only the loopback and
	 * reset (AN) bits in the control register (0x0) can be written.
	 * These functionalities have custom implementations in the PHY
	 * driver. Since the MAC and PHY are integrated in the same device, we
	 * can read the OA TC6 MACPHY ID register instead of the PHYID (0x2
	 * and 0x3) ones, as their value matches. C45 accesses do not cause
	 * this issue.
	 */

	switch (regnum) {
	case MII_BMCR:
		return ADIN1140_PHY_CTRL_DEFAULT;
	case MII_BMSR:
		return ADIN1140_PHY_STATUS_DEFAULT;
	case MII_PHYSID1:
		ret = oa_tc6_read_register(tc6, ADIN1140_MACPHY_ID_REG,
					   &reg_val);
		if (ret)
			return ret;

		return FIELD_GET(GENMASK(31, 16), reg_val);
	case MII_PHYSID2:
		ret = oa_tc6_read_register(tc6, ADIN1140_MACPHY_ID_REG,
					   &reg_val);
		if (ret)
			return ret;

		return FIELD_GET(GENMASK(15, 0), reg_val);
	default:
		return 0xFFFF;
	}
}

static int adin1140_mdiobus_write(struct mii_bus *bus, int addr, int regnum,
				  u16 val)
{
	return 0;
}

static int adin1140_mdio_register(struct adin1140_priv *priv)
{
	priv->mdiobus = mdiobus_alloc();
	if (!priv->mdiobus) {
		netdev_err(priv->netdev, "MDIO bus alloc failed\n");
		return -ENOMEM;
	}

	priv->mdiobus->read = adin1140_mdiobus_read;
	priv->mdiobus->write = adin1140_mdiobus_write;
	priv->mdiobus->read_c45 = adin1140_mdiobus_read_c45;
	priv->mdiobus->write_c45 = adin1140_mdiobus_write_c45;

	return 0;
}

static const struct ethtool_ops adin1140_ethtool_ops = {
	.get_drvinfo = adin1140_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_ethtool_stats = adin1140_get_ethtool_stats,
	.get_sset_count = adin1140_get_sset_count,
	.get_strings = adin1140_get_ethtool_strings,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
};

static const struct net_device_ops adin1140_netdev_ops = {
	.ndo_open = adin1140_open,
	.ndo_stop = adin1140_close,
	.ndo_start_xmit	= adin1140_start_xmit,
	.ndo_set_mac_address = adin1140_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_rx_mode = adin1140_rx_mode,
	.ndo_eth_ioctl = adin1140_ioctl,
	.ndo_get_stats64 = adin1140_ndo_get_stats64,
};

static int adin1140_probe(struct spi_device *spi)
{
	struct oa_tc6_config tc6_config = {};
	struct net_device *netdev;
	struct adin1140_priv *priv;
	int ret;

	netdev = alloc_etherdev(sizeof(struct adin1140_priv));
	if (!netdev)
		return -ENOMEM;

	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	spi_set_drvdata(spi, priv);
	spin_lock_init(&priv->stat_lock);

	ret = adin1140_mdio_register(priv);
	if (ret)
		goto netdev_free;

	tc6_config.spi = spi;
	tc6_config.netdev = netdev;
	tc6_config.mii_bus = priv->mdiobus;

	priv->tc6 = oa_tc6_init(&tc6_config);
	if (!priv->tc6) {
		ret = -ENODEV;
		goto mdio_free;
	}

	if (device_get_ethdev_address(&spi->dev, netdev))
		eth_hw_addr_random(netdev);

	ret = adin1140_configure(priv);
	if (ret)
		goto oa_tc6_exit;

	INIT_WORK(&priv->rx_mode_work, adin1140_rx_mode_work);
	INIT_DELAYED_WORK(&priv->stats_work, adin1140_stats_work);

	netdev->if_port = IF_PORT_10BASET;
	netdev->irq = spi->irq;
	netdev->netdev_ops = &adin1140_netdev_ops;
	netdev->ethtool_ops = &adin1140_ethtool_ops;
	netdev->netns_immutable = true;
	netdev->priv_flags |= IFF_LIVE_ADDR_CHANGE |
			      IFF_UNICAST_FLT;

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(&spi->dev, "Failed to register netdev (%d)", ret);
		goto oa_tc6_exit;
	}

	return 0;

oa_tc6_exit:
	oa_tc6_exit(priv->tc6);
mdio_free:
	mdiobus_free(priv->mdiobus);
netdev_free:
	free_netdev(priv->netdev);

	return ret;
}

static void adin1140_remove(struct spi_device *spi)
{
	struct adin1140_priv *priv = spi_get_drvdata(spi);

	cancel_work_sync(&priv->rx_mode_work);
	unregister_netdev(priv->netdev);
	oa_tc6_exit(priv->tc6);
	mdiobus_free(priv->mdiobus);
	free_netdev(priv->netdev);
}

static const struct spi_device_id adin1140_spi_id[] = {
	{ .name = "adin1140" },
	{ .name = "ad3306" },
	{},
};
MODULE_DEVICE_TABLE(spi, adin1140_spi_id);

static const struct of_device_id adin1140_match_table[] = {
	{ .compatible = "adi,adin1140" },
	{ .compatible = "adi,ad3306" },
	{ }
};
MODULE_DEVICE_TABLE(of, adin1140_match_table);

static struct spi_driver adin1140_driver = {
	.driver = {
		.name = "adin1140",
		.of_match_table = adin1140_match_table,
	 },
	.probe = adin1140_probe,
	.remove = adin1140_remove,
	.id_table = adin1140_spi_id,
};
module_spi_driver(adin1140_driver);

MODULE_DESCRIPTION("Analog Devices, Inc. ADIN1140 10BASE-T1S MAC-PHY");
MODULE_AUTHOR("Ciprian Regus <ciprian.regus@analog.com>");
MODULE_LICENSE("GPL");
