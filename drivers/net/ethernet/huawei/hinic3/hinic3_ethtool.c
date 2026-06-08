// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/ethtool.h>

#include "hinic3_lld.h"
#include "hinic3_hw_comm.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"

#define HINIC3_MGMT_VERSION_MAX_LEN     32
/* Coalesce time properties in microseconds */
#define COALESCE_PENDING_LIMIT_UNIT     8
#define COALESCE_TIMER_CFG_UNIT         5
#define COALESCE_MAX_PENDING_LIMIT      (255 * COALESCE_PENDING_LIMIT_UNIT)
#define COALESCE_MAX_TIMER_CFG          (255 * COALESCE_TIMER_CFG_UNIT)

static void hinic3_get_drvinfo(struct net_device *netdev,
			       struct ethtool_drvinfo *info)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u8 mgmt_ver[HINIC3_MGMT_VERSION_MAX_LEN];
	struct pci_dev *pdev = nic_dev->pdev;
	int err;

	strscpy(info->driver, HINIC3_NIC_DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, pci_name(pdev), sizeof(info->bus_info));

	err = hinic3_get_mgmt_version(nic_dev->hwdev, mgmt_ver,
				      HINIC3_MGMT_VERSION_MAX_LEN);
	if (err) {
		netdev_err(netdev, "Failed to get fw version\n");
		return;
	}

	snprintf(info->fw_version, sizeof(info->fw_version), "%s", mgmt_ver);
}

static u32 hinic3_get_msglevel(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	return nic_dev->msg_enable;
}

static void hinic3_set_msglevel(struct net_device *netdev, u32 data)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	nic_dev->msg_enable = data;

	netdev_dbg(netdev, "Set message level: 0x%x\n", data);
}

static const u32 hinic3_link_mode_ge[] = {
	ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
	ETHTOOL_LINK_MODE_1000baseKX_Full_BIT,
	ETHTOOL_LINK_MODE_1000baseX_Full_BIT,
};

static const u32 hinic3_link_mode_10ge_base_r[] = {
	ETHTOOL_LINK_MODE_10000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseR_FEC_BIT,
	ETHTOOL_LINK_MODE_10000baseCR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseSR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseLR_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseLRM_Full_BIT,
};

static const u32 hinic3_link_mode_25ge_base_r[] = {
	ETHTOOL_LINK_MODE_25000baseCR_Full_BIT,
	ETHTOOL_LINK_MODE_25000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_25000baseSR_Full_BIT,
};

static const u32 hinic3_link_mode_40ge_base_r4[] = {
	ETHTOOL_LINK_MODE_40000baseKR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseCR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_40000baseLR4_Full_BIT,
};

static const u32 hinic3_link_mode_50ge_base_r[] = {
	ETHTOOL_LINK_MODE_50000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseSR_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseCR_Full_BIT,
};

static const u32 hinic3_link_mode_50ge_base_r2[] = {
	ETHTOOL_LINK_MODE_50000baseCR2_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseKR2_Full_BIT,
	ETHTOOL_LINK_MODE_50000baseSR2_Full_BIT,
};

static const u32 hinic3_link_mode_100ge_base_r[] = {
	ETHTOOL_LINK_MODE_100000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseSR_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseCR_Full_BIT,
};

static const u32 hinic3_link_mode_100ge_base_r2[] = {
	ETHTOOL_LINK_MODE_100000baseKR2_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseSR2_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseCR2_Full_BIT,
};

static const u32 hinic3_link_mode_100ge_base_r4[] = {
	ETHTOOL_LINK_MODE_100000baseKR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseCR4_Full_BIT,
	ETHTOOL_LINK_MODE_100000baseLR4_ER4_Full_BIT,
};

static const u32 hinic3_link_mode_200ge_base_r2[] = {
	ETHTOOL_LINK_MODE_200000baseKR2_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseSR2_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseCR2_Full_BIT,
};

static const u32 hinic3_link_mode_200ge_base_r4[] = {
	ETHTOOL_LINK_MODE_200000baseKR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseSR4_Full_BIT,
	ETHTOOL_LINK_MODE_200000baseCR4_Full_BIT,
};

struct hw2ethtool_link_mode {
	const u32 *link_mode_bit_arr;
	u32       arr_size;
	u32       speed;
};

static const struct hw2ethtool_link_mode
	hw2ethtool_link_mode_table[LINK_MODE_MAX_NUMBERS] = {
	[LINK_MODE_GE] = {
		.link_mode_bit_arr = hinic3_link_mode_ge,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_ge),
		.speed             = SPEED_1000,
	},
	[LINK_MODE_10GE_BASE_R] = {
		.link_mode_bit_arr = hinic3_link_mode_10ge_base_r,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_10ge_base_r),
		.speed             = SPEED_10000,
	},
	[LINK_MODE_25GE_BASE_R] = {
		.link_mode_bit_arr = hinic3_link_mode_25ge_base_r,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_25ge_base_r),
		.speed             = SPEED_25000,
	},
	[LINK_MODE_40GE_BASE_R4] = {
		.link_mode_bit_arr = hinic3_link_mode_40ge_base_r4,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_40ge_base_r4),
		.speed             = SPEED_40000,
	},
	[LINK_MODE_50GE_BASE_R] = {
		.link_mode_bit_arr = hinic3_link_mode_50ge_base_r,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_50ge_base_r),
		.speed             = SPEED_50000,
	},
	[LINK_MODE_50GE_BASE_R2] = {
		.link_mode_bit_arr = hinic3_link_mode_50ge_base_r2,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_50ge_base_r2),
		.speed             = SPEED_50000,
	},
	[LINK_MODE_100GE_BASE_R] = {
		.link_mode_bit_arr = hinic3_link_mode_100ge_base_r,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_100ge_base_r),
		.speed             = SPEED_100000,
	},
	[LINK_MODE_100GE_BASE_R2] = {
		.link_mode_bit_arr = hinic3_link_mode_100ge_base_r2,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_100ge_base_r2),
		.speed             = SPEED_100000,
	},
	[LINK_MODE_100GE_BASE_R4] = {
		.link_mode_bit_arr = hinic3_link_mode_100ge_base_r4,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_100ge_base_r4),
		.speed             = SPEED_100000,
	},
	[LINK_MODE_200GE_BASE_R2] = {
		.link_mode_bit_arr = hinic3_link_mode_200ge_base_r2,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_200ge_base_r2),
		.speed             = SPEED_200000,
	},
	[LINK_MODE_200GE_BASE_R4] = {
		.link_mode_bit_arr = hinic3_link_mode_200ge_base_r4,
		.arr_size          = ARRAY_SIZE(hinic3_link_mode_200ge_base_r4),
		.speed             = SPEED_200000,
	},
};

#define GET_SUPPORTED_MODE     0
#define GET_ADVERTISED_MODE    1

struct hinic3_link_settings {
	__ETHTOOL_DECLARE_LINK_MODE_MASK(supported);
	__ETHTOOL_DECLARE_LINK_MODE_MASK(advertising);

	u32 speed;
	u8  duplex;
	u8  port;
	u8  autoneg;
};

#define HINIC3_ADD_SUPPORTED_LINK_MODE(ecmd, mode) \
	set_bit(ETHTOOL_LINK_##mode##_BIT, (ecmd)->supported)
#define HINIC3_ADD_ADVERTISED_LINK_MODE(ecmd, mode) \
	set_bit(ETHTOOL_LINK_##mode##_BIT, (ecmd)->advertising)

static void hinic3_add_speed_link_mode(unsigned long *bitmap, u32 mode)
{
	u32 i;

	for (i = 0; i < hw2ethtool_link_mode_table[mode].arr_size; i++) {
		if (hw2ethtool_link_mode_table[mode].link_mode_bit_arr[i] >=
		    __ETHTOOL_LINK_MODE_MASK_NBITS)
			continue;

		set_bit(hw2ethtool_link_mode_table[mode].link_mode_bit_arr[i],
			bitmap);
	}
}

/* Related to enum mag_cmd_port_speed */
static const u32 hw_to_ethtool_speed[] = {
	(u32)SPEED_UNKNOWN, SPEED_10,    SPEED_100,   SPEED_1000,   SPEED_10000,
	SPEED_25000,        SPEED_40000, SPEED_50000, SPEED_100000, SPEED_200000
};

static void
hinic3_add_ethtool_link_mode(struct hinic3_link_settings *link_settings,
			     u32 hw_link_mode, u32 name)
{
	unsigned long *advertising_mask = link_settings->advertising;
	unsigned long *supported_mask = link_settings->supported;
	u32 link_mode;

	for (link_mode = 0; link_mode < LINK_MODE_MAX_NUMBERS; link_mode++) {
		if (hw_link_mode & BIT(link_mode)) {
			if (name == GET_SUPPORTED_MODE)
				hinic3_add_speed_link_mode(supported_mask,
							   link_mode);
			else
				hinic3_add_speed_link_mode(advertising_mask,
							   link_mode);
		}
	}
}

static void
hinic3_link_speed_set(struct net_device *netdev,
		      struct hinic3_link_settings *link_settings,
		      struct hinic3_nic_port_info *port_info)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	bool link_status_up;
	int err;

	if (port_info->supported_mode != LINK_MODE_UNKNOWN)
		hinic3_add_ethtool_link_mode(link_settings,
					     port_info->supported_mode,
					     GET_SUPPORTED_MODE);
	if (port_info->advertised_mode != LINK_MODE_UNKNOWN)
		hinic3_add_ethtool_link_mode(link_settings,
					     port_info->advertised_mode,
					     GET_ADVERTISED_MODE);

	err = hinic3_get_link_status(nic_dev->hwdev, &link_status_up);
	if (!err && link_status_up) {
		link_settings->speed =
			port_info->speed < ARRAY_SIZE(hw_to_ethtool_speed) ?
			hw_to_ethtool_speed[port_info->speed] :
			(u32)SPEED_UNKNOWN;

		link_settings->duplex = port_info->duplex;
	} else {
		link_settings->speed = (u32)SPEED_UNKNOWN;
		link_settings->duplex = DUPLEX_UNKNOWN;
	}
}

static void
hinic3_link_port_type_set(struct hinic3_link_settings *link_settings,
			  u8 port_type)
{
	switch (port_type) {
	case MAG_CMD_WIRE_TYPE_ELECTRIC:
		HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_TP);
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_TP);
		link_settings->port = PORT_TP;
		break;

	case MAG_CMD_WIRE_TYPE_AOC:
	case MAG_CMD_WIRE_TYPE_MM:
	case MAG_CMD_WIRE_TYPE_SM:
		HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_FIBRE);
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_FIBRE);
		link_settings->port = PORT_FIBRE;
		break;

	case MAG_CMD_WIRE_TYPE_COPPER:
		HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_FIBRE);
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_FIBRE);
		link_settings->port = PORT_DA;
		break;

	case MAG_CMD_WIRE_TYPE_BACKPLANE:
		HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_Backplane);
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_Backplane);
		link_settings->port = PORT_NONE;
		break;

	default:
		link_settings->port = PORT_OTHER;
		break;
	}
}

static int
hinic3_get_link_pause_settings(struct net_device *netdev,
			       struct hinic3_link_settings *link_settings)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_nic_pause_config nic_pause = {};
	int err;

	err = hinic3_get_pause_info(nic_dev, &nic_pause);
	if (err) {
		netdev_err(netdev, "Failed to get pause param from hw\n");
		return err;
	}

	HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_Pause);
	if (nic_pause.rx_pause && nic_pause.tx_pause) {
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_Pause);
	} else if (nic_pause.tx_pause) {
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings,
						MODE_Asym_Pause);
	} else if (nic_pause.rx_pause) {
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_Pause);
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings,
						MODE_Asym_Pause);
	}

	return 0;
}

static int
hinic3_get_link_settings(struct net_device *netdev,
			 struct hinic3_link_settings *link_settings)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_nic_port_info port_info = {};
	int err;

	err = hinic3_get_port_info(nic_dev->hwdev, &port_info);
	if (err) {
		netdev_err(netdev, "Failed to get port info\n");
		return err;
	}

	hinic3_link_speed_set(netdev, link_settings, &port_info);

	hinic3_link_port_type_set(link_settings, port_info.port_type);

	link_settings->autoneg = port_info.autoneg_state == PORT_CFG_AN_ON ?
				 AUTONEG_ENABLE : AUTONEG_DISABLE;
	if (port_info.autoneg_cap)
		HINIC3_ADD_SUPPORTED_LINK_MODE(link_settings, MODE_Autoneg);
	if (port_info.autoneg_state == PORT_CFG_AN_ON)
		HINIC3_ADD_ADVERTISED_LINK_MODE(link_settings, MODE_Autoneg);

	if (!HINIC3_IS_VF(nic_dev->hwdev)) {
		err = hinic3_get_link_pause_settings(netdev, link_settings);
		if (err)
			return err;
	}

	return 0;
}

static int
hinic3_get_link_ksettings(struct net_device *netdev,
			  struct ethtool_link_ksettings *link_settings)
{
	struct ethtool_link_settings *base = &link_settings->base;
	struct hinic3_link_settings settings = {};
	int err;

	ethtool_link_ksettings_zero_link_mode(link_settings, supported);
	ethtool_link_ksettings_zero_link_mode(link_settings, advertising);

	err = hinic3_get_link_settings(netdev, &settings);
	if (err)
		return err;

	bitmap_copy(link_settings->link_modes.supported, settings.supported,
		    __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_copy(link_settings->link_modes.advertising, settings.advertising,
		    __ETHTOOL_LINK_MODE_MASK_NBITS);

	base->autoneg = settings.autoneg;
	base->speed = settings.speed;
	base->duplex = settings.duplex;
	base->port = settings.port;

	return 0;
}

static void hinic3_get_ringparam(struct net_device *netdev,
				 struct ethtool_ringparam *ring,
				 struct kernel_ethtool_ringparam *kernel_ring,
				 struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	ring->rx_max_pending = HINIC3_MAX_RX_QUEUE_DEPTH;
	ring->tx_max_pending = HINIC3_MAX_TX_QUEUE_DEPTH;
	ring->rx_pending = nic_dev->q_params.rq_depth;
	ring->rx_pending = nic_dev->q_params.sq_depth;
}

static void hinic3_update_qp_depth(struct net_device *netdev,
				   u32 sq_depth, u32 rq_depth)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 i;

	nic_dev->q_params.sq_depth = sq_depth;
	nic_dev->q_params.rq_depth = rq_depth;
	for (i = 0; i < nic_dev->max_qps; i++) {
		nic_dev->txqs[i].q_depth = sq_depth;
		nic_dev->txqs[i].q_mask = sq_depth - 1;
		nic_dev->rxqs[i].q_depth = rq_depth;
		nic_dev->rxqs[i].q_mask = rq_depth - 1;
	}
}

static int hinic3_check_ringparam_valid(struct net_device *netdev,
					const struct ethtool_ringparam *ring,
					struct netlink_ext_ack *extack)
{
	if (ring->tx_pending < HINIC3_MIN_QUEUE_DEPTH ||
	    ring->rx_pending < HINIC3_MIN_QUEUE_DEPTH) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "Queue depth out of range tx[%d-%d] rx[%d-%d]",
				       HINIC3_MIN_QUEUE_DEPTH,
				       HINIC3_MAX_TX_QUEUE_DEPTH,
				       HINIC3_MIN_QUEUE_DEPTH,
				       HINIC3_MAX_RX_QUEUE_DEPTH);

		return -EINVAL;
	}

	return 0;
}

static int hinic3_set_ringparam(struct net_device *netdev,
				struct ethtool_ringparam *ring,
				struct kernel_ethtool_ringparam *kernel_ring,
				struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_dyna_txrxq_params q_params = {};
	u32 new_sq_depth, new_rq_depth;
	int err;

	err = hinic3_check_ringparam_valid(netdev, ring, extack);
	if (err)
		return err;

	new_sq_depth = 1U << ilog2(ring->tx_pending);
	new_rq_depth = 1U << ilog2(ring->rx_pending);
	if (new_sq_depth == nic_dev->q_params.sq_depth &&
	    new_rq_depth == nic_dev->q_params.rq_depth)
		return 0;

	if (new_sq_depth != ring->tx_pending ||
	    new_rq_depth != ring->rx_pending)
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "Requested Tx/Rx ring depth %u/%u trimmed to %u/%u",
				       ring->tx_pending, ring->rx_pending,
				       new_sq_depth, new_rq_depth);

	netdev_info(netdev, "Change Tx/Rx ring depth from %u/%u to %u/%u\n",
		    nic_dev->q_params.sq_depth, nic_dev->q_params.rq_depth,
		    new_sq_depth, new_rq_depth);

	if (!netif_running(netdev)) {
		hinic3_update_qp_depth(netdev, new_sq_depth, new_rq_depth);
	} else {
		q_params = nic_dev->q_params;
		q_params.sq_depth = new_sq_depth;
		q_params.rq_depth = new_rq_depth;

		err = hinic3_change_channel_settings(netdev, &q_params);
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to change channel settings");
			return err;
		}
	}

	return 0;
}

struct hinic3_stats {
	char name[ETH_GSTRING_LEN];
	u32  size;
	int  offset;
};

#define HINIC3_RXQ_STAT(_stat_item) { \
	.name   = "rxq%d_"#_stat_item, \
	.size   = sizeof_field(struct hinic3_rxq_stats, _stat_item), \
	.offset = offsetof(struct hinic3_rxq_stats, _stat_item) \
}

#define HINIC3_TXQ_STAT(_stat_item) { \
	.name   = "txq%d_"#_stat_item, \
	.size   = sizeof_field(struct hinic3_txq_stats, _stat_item), \
	.offset = offsetof(struct hinic3_txq_stats, _stat_item) \
}

static struct hinic3_stats hinic3_rx_queue_stats[] = {
	HINIC3_RXQ_STAT(csum_errors),
	HINIC3_RXQ_STAT(other_errors),
	HINIC3_RXQ_STAT(rx_buf_empty),
	HINIC3_RXQ_STAT(alloc_skb_err),
	HINIC3_RXQ_STAT(alloc_rx_buf_err),
};

static struct hinic3_stats hinic3_tx_queue_stats[] = {
	HINIC3_TXQ_STAT(busy),
	HINIC3_TXQ_STAT(skb_pad_err),
	HINIC3_TXQ_STAT(frag_len_overflow),
	HINIC3_TXQ_STAT(offload_cow_skb_err),
	HINIC3_TXQ_STAT(map_frag_err),
	HINIC3_TXQ_STAT(unknown_tunnel_pkt),
	HINIC3_TXQ_STAT(frag_size_err),
};

#define HINIC3_FUNC_STAT(_stat_item) {	\
	.name   = #_stat_item, \
	.size   = sizeof_field(struct l2nic_vport_stats, _stat_item), \
	.offset = offsetof(struct l2nic_vport_stats, _stat_item) \
}

static struct hinic3_stats hinic3_function_stats[] = {
	HINIC3_FUNC_STAT(tx_unicast_pkts_vport),
	HINIC3_FUNC_STAT(tx_unicast_bytes_vport),
	HINIC3_FUNC_STAT(tx_multicast_pkts_vport),
	HINIC3_FUNC_STAT(tx_multicast_bytes_vport),
	HINIC3_FUNC_STAT(tx_broadcast_pkts_vport),
	HINIC3_FUNC_STAT(tx_broadcast_bytes_vport),

	HINIC3_FUNC_STAT(rx_unicast_pkts_vport),
	HINIC3_FUNC_STAT(rx_unicast_bytes_vport),
	HINIC3_FUNC_STAT(rx_multicast_pkts_vport),
	HINIC3_FUNC_STAT(rx_multicast_bytes_vport),
	HINIC3_FUNC_STAT(rx_broadcast_pkts_vport),
	HINIC3_FUNC_STAT(rx_broadcast_bytes_vport),

	HINIC3_FUNC_STAT(tx_discard_vport),
	HINIC3_FUNC_STAT(rx_discard_vport),
	HINIC3_FUNC_STAT(tx_err_vport),
	HINIC3_FUNC_STAT(rx_err_vport),
};

#define HINIC3_PORT_STAT(_stat_item) { \
	.name   = #_stat_item, \
	.size   = sizeof_field(struct mag_cmd_port_stats, _stat_item), \
	.offset = offsetof(struct mag_cmd_port_stats, _stat_item) \
}

static struct hinic3_stats hinic3_port_stats[] = {
	HINIC3_PORT_STAT(mac_tx_fragment_pkt_num),
	HINIC3_PORT_STAT(mac_tx_undersize_pkt_num),
	HINIC3_PORT_STAT(mac_tx_undermin_pkt_num),
	HINIC3_PORT_STAT(mac_tx_1519_max_bad_pkt_num),
	HINIC3_PORT_STAT(mac_tx_1519_max_good_pkt_num),
	HINIC3_PORT_STAT(mac_tx_oversize_pkt_num),
	HINIC3_PORT_STAT(mac_tx_jabber_pkt_num),
	HINIC3_PORT_STAT(mac_tx_bad_pkt_num),
	HINIC3_PORT_STAT(mac_tx_bad_oct_num),
	HINIC3_PORT_STAT(mac_tx_good_oct_num),
	HINIC3_PORT_STAT(mac_tx_total_pkt_num),
	HINIC3_PORT_STAT(mac_tx_uni_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri0_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri1_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri2_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri3_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri4_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri5_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri6_pkt_num),
	HINIC3_PORT_STAT(mac_tx_pfc_pri7_pkt_num),
	HINIC3_PORT_STAT(mac_tx_err_all_pkt_num),
	HINIC3_PORT_STAT(mac_tx_from_app_good_pkt_num),
	HINIC3_PORT_STAT(mac_tx_from_app_bad_pkt_num),

	HINIC3_PORT_STAT(mac_rx_undermin_pkt_num),
	HINIC3_PORT_STAT(mac_rx_1519_max_bad_pkt_num),
	HINIC3_PORT_STAT(mac_rx_1519_max_good_pkt_num),
	HINIC3_PORT_STAT(mac_rx_bad_pkt_num),
	HINIC3_PORT_STAT(mac_rx_bad_oct_num),
	HINIC3_PORT_STAT(mac_rx_good_oct_num),
	HINIC3_PORT_STAT(mac_rx_total_pkt_num),
	HINIC3_PORT_STAT(mac_rx_uni_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri0_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri1_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri2_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri3_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri4_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri5_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri6_pkt_num),
	HINIC3_PORT_STAT(mac_rx_pfc_pri7_pkt_num),
	HINIC3_PORT_STAT(mac_rx_send_app_good_pkt_num),
	HINIC3_PORT_STAT(mac_rx_send_app_bad_pkt_num),
	HINIC3_PORT_STAT(mac_rx_unfilter_pkt_num),
};

static int hinic3_get_sset_count(struct net_device *netdev, int sset)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int count, q_num;

	switch (sset) {
	case ETH_SS_STATS:
		q_num = nic_dev->q_params.num_qps;
		count = ARRAY_SIZE(hinic3_function_stats) +
			(ARRAY_SIZE(hinic3_tx_queue_stats) +
			 ARRAY_SIZE(hinic3_rx_queue_stats)) *
			q_num;

		if (!HINIC3_IS_VF(nic_dev->hwdev))
			count += ARRAY_SIZE(hinic3_port_stats);

		return count;
	default:
		return -EOPNOTSUPP;
	}
}

static u64 get_val_of_ptr(u32 size, const void *ptr)
{
	u64 ret = size == sizeof(u64) ? *(u64 *)ptr :
		  size == sizeof(u32) ? *(u32 *)ptr :
		  size == sizeof(u16) ? *(u16 *)ptr :
		  *(u8 *)ptr;

	return ret;
}

static void hinic3_get_drv_queue_stats(struct net_device *netdev, u64 *data)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_txq_stats txq_stats = {};
	struct hinic3_rxq_stats rxq_stats = {};
	u16 i = 0, j, qid;
	char *p;

	for (qid = 0; qid < nic_dev->q_params.num_qps; qid++) {
		if (!nic_dev->txqs)
			break;

		hinic3_txq_get_stats(&nic_dev->txqs[qid], &txq_stats);
		for (j = 0; j < ARRAY_SIZE(hinic3_tx_queue_stats); j++, i++) {
			p = (char *)&txq_stats +
			    hinic3_tx_queue_stats[j].offset;
			data[i] = get_val_of_ptr(hinic3_tx_queue_stats[j].size,
						 p);
		}
	}

	i = nic_dev->q_params.num_qps * ARRAY_SIZE(hinic3_tx_queue_stats);
	for (qid = 0; qid < nic_dev->q_params.num_qps; qid++) {
		if (!nic_dev->rxqs)
			break;

		hinic3_rxq_get_stats(&nic_dev->rxqs[qid], &rxq_stats);
		for (j = 0; j < ARRAY_SIZE(hinic3_rx_queue_stats); j++, i++) {
			p = (char *)&rxq_stats +
			    hinic3_rx_queue_stats[j].offset;
			data[i] = get_val_of_ptr(hinic3_rx_queue_stats[j].size,
						 p);
		}
	}
}

static u16 hinic3_get_ethtool_port_stats(struct net_device *netdev, u64 *data)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	u16 i = 0, j;
	char *p;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		goto err_zero_stats;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get port stats from fw\n");
		goto err_zero_stats;
	}

	for (j = 0; j < ARRAY_SIZE(hinic3_port_stats); j++, i++) {
		p = (char *)ps + hinic3_port_stats[j].offset;
		data[i] = get_val_of_ptr(hinic3_port_stats[j].size, p);
	}

	kfree(ps);

	return i;

err_zero_stats:
	memset(&data[i], 0, ARRAY_SIZE(hinic3_port_stats) * sizeof(*data));

	return i + ARRAY_SIZE(hinic3_port_stats);
}

static void hinic3_get_ethtool_stats(struct net_device *netdev,
				     struct ethtool_stats *stats, u64 *data)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct l2nic_vport_stats vport_stats = {};
	u16 i = 0, j;
	char *p;
	int err;

	err = hinic3_get_vport_stats(nic_dev->hwdev,
				     hinic3_global_func_id(nic_dev->hwdev),
				     &vport_stats);
	if (err)
		netdev_err(netdev, "Failed to get function stats from fw\n");

	for (j = 0; j < ARRAY_SIZE(hinic3_function_stats); j++, i++) {
		p = (char *)&vport_stats + hinic3_function_stats[j].offset;
		data[i] = get_val_of_ptr(hinic3_function_stats[j].size, p);
	}

	if (!HINIC3_IS_VF(nic_dev->hwdev))
		i += hinic3_get_ethtool_port_stats(netdev, data + i);

	hinic3_get_drv_queue_stats(netdev, data + i);
}

static u16 hinic3_get_hw_stats_strings(struct net_device *netdev, char *p)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u16 i, cnt = 0;

	for (i = 0; i < ARRAY_SIZE(hinic3_function_stats); i++) {
		memcpy(p, hinic3_function_stats[i].name, ETH_GSTRING_LEN);
		p += ETH_GSTRING_LEN;
		cnt++;
	}

	if (!HINIC3_IS_VF(nic_dev->hwdev)) {
		for (i = 0; i < ARRAY_SIZE(hinic3_port_stats); i++) {
			memcpy(p, hinic3_port_stats[i].name, ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
			cnt++;
		}
	}

	return cnt;
}

static void hinic3_get_qp_stats_strings(struct net_device *netdev, char *p)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	u8 *data = p;
	u16 i, j;

	for (i = 0; i < nic_dev->q_params.num_qps; i++) {
		for (j = 0; j < ARRAY_SIZE(hinic3_tx_queue_stats); j++)
			ethtool_sprintf(&data,
					hinic3_tx_queue_stats[j].name, i);
	}

	for (i = 0; i < nic_dev->q_params.num_qps; i++) {
		for (j = 0; j < ARRAY_SIZE(hinic3_rx_queue_stats); j++)
			ethtool_sprintf(&data,
					hinic3_rx_queue_stats[j].name, i);
	}
}

static void hinic3_get_strings(struct net_device *netdev,
			       u32 stringset, u8 *data)
{
	char *p = (char *)data;
	u16 offset;

	switch (stringset) {
	case ETH_SS_STATS:
		offset = hinic3_get_hw_stats_strings(netdev, p);
		hinic3_get_qp_stats_strings(netdev,
					    p + offset * ETH_GSTRING_LEN);

		return;
	default:
		netdev_err(netdev, "Invalid string set %u.\n", stringset);
		return;
	}
}

static void hinic3_get_eth_phy_stats(struct net_device *netdev,
				     struct ethtool_eth_phy_stats *phy_stats)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		return;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get eth phy stats from fw\n");
		return;
	}

	phy_stats->SymbolErrorDuringCarrier = ps->mac_rx_sym_err_pkt_num;

	kfree(ps);
}

static void hinic3_get_eth_mac_stats(struct net_device *netdev,
				     struct ethtool_eth_mac_stats *mac_stats)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		return;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get eth mac stats from fw\n");
		return;
	}

	mac_stats->FramesTransmittedOK = ps->mac_tx_good_pkt_num;
	mac_stats->FramesReceivedOK = ps->mac_rx_good_pkt_num;
	mac_stats->FrameCheckSequenceErrors = ps->mac_rx_fcs_err_pkt_num;
	mac_stats->OctetsTransmittedOK = ps->mac_tx_total_oct_num;
	mac_stats->OctetsReceivedOK = ps->mac_rx_total_oct_num;
	mac_stats->MulticastFramesXmittedOK = ps->mac_tx_multi_pkt_num;
	mac_stats->BroadcastFramesXmittedOK = ps->mac_tx_broad_pkt_num;
	mac_stats->MulticastFramesReceivedOK = ps->mac_rx_multi_pkt_num;
	mac_stats->BroadcastFramesReceivedOK = ps->mac_rx_broad_pkt_num;

	kfree(ps);
}

static void hinic3_get_eth_ctrl_stats(struct net_device *netdev,
				      struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		return;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get eth ctrl stats from fw\n");
		return;
	}

	ctrl_stats->MACControlFramesTransmitted = ps->mac_tx_control_pkt_num;
	ctrl_stats->MACControlFramesReceived = ps->mac_rx_control_pkt_num;

	kfree(ps);
}

static const struct ethtool_rmon_hist_range hinic3_rmon_ranges[] = {
	{     0,    64 },
	{    65,   127 },
	{   128,   255 },
	{   256,   511 },
	{   512,  1023 },
	{  1024,  1518 },
	{  1519,  2047 },
	{  2048,  4095 },
	{  4096,  8191 },
	{  8192,  9216 },
	{  9217, 12287 },
	{}
};

static void hinic3_get_rmon_stats(struct net_device *netdev,
				  struct ethtool_rmon_stats *rmon_stats,
				  const struct ethtool_rmon_hist_range **ranges)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		return;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get eth rmon stats from fw\n");
		return;
	}

	rmon_stats->undersize_pkts	= ps->mac_rx_undersize_pkt_num;
	rmon_stats->oversize_pkts	= ps->mac_rx_oversize_pkt_num;
	rmon_stats->fragments		= ps->mac_rx_fragment_pkt_num;
	rmon_stats->jabbers		= ps->mac_rx_jabber_pkt_num;

	rmon_stats->hist[0]		= ps->mac_rx_64_oct_pkt_num;
	rmon_stats->hist[1]		= ps->mac_rx_65_127_oct_pkt_num;
	rmon_stats->hist[2]		= ps->mac_rx_128_255_oct_pkt_num;
	rmon_stats->hist[3]		= ps->mac_rx_256_511_oct_pkt_num;
	rmon_stats->hist[4]		= ps->mac_rx_512_1023_oct_pkt_num;
	rmon_stats->hist[5]		= ps->mac_rx_1024_1518_oct_pkt_num;
	rmon_stats->hist[6]		= ps->mac_rx_1519_2047_oct_pkt_num;
	rmon_stats->hist[7]		= ps->mac_rx_2048_4095_oct_pkt_num;
	rmon_stats->hist[8]		= ps->mac_rx_4096_8191_oct_pkt_num;
	rmon_stats->hist[9]		= ps->mac_rx_8192_9216_oct_pkt_num;
	rmon_stats->hist[10]		= ps->mac_rx_9217_12287_oct_pkt_num;

	rmon_stats->hist_tx[0]		= ps->mac_tx_64_oct_pkt_num;
	rmon_stats->hist_tx[1]		= ps->mac_tx_65_127_oct_pkt_num;
	rmon_stats->hist_tx[2]		= ps->mac_tx_128_255_oct_pkt_num;
	rmon_stats->hist_tx[3]		= ps->mac_tx_256_511_oct_pkt_num;
	rmon_stats->hist_tx[4]		= ps->mac_tx_512_1023_oct_pkt_num;
	rmon_stats->hist_tx[5]		= ps->mac_tx_1024_1518_oct_pkt_num;
	rmon_stats->hist_tx[6]		= ps->mac_tx_1519_2047_oct_pkt_num;
	rmon_stats->hist_tx[7]		= ps->mac_tx_2048_4095_oct_pkt_num;
	rmon_stats->hist_tx[8]		= ps->mac_tx_4096_8191_oct_pkt_num;
	rmon_stats->hist_tx[9]		= ps->mac_tx_8192_9216_oct_pkt_num;
	rmon_stats->hist_tx[10]		= ps->mac_tx_9217_12287_oct_pkt_num;

	*ranges = hinic3_rmon_ranges;

	kfree(ps);
}

static void hinic3_get_pause_stats(struct net_device *netdev,
				   struct ethtool_pause_stats *pause_stats)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct mag_cmd_port_stats *ps;
	int err;

	ps = kmalloc_obj(*ps);
	if (!ps)
		return;

	err = hinic3_get_phy_port_stats(nic_dev->hwdev, ps);
	if (err) {
		kfree(ps);
		netdev_err(netdev, "Failed to get eth pause stats from fw\n");
		return;
	}

	pause_stats->tx_pause_frames = ps->mac_tx_pause_num;
	pause_stats->rx_pause_frames = ps->mac_rx_pause_num;

	kfree(ps);
}

static int hinic3_set_queue_coalesce(struct net_device *netdev, u16 q_id,
				     struct hinic3_intr_coal_info *coal,
				     struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *intr_coal;
	struct hinic3_interrupt_info info = {};
	int err;

	if (nic_dev->adaptive_rx_coal) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Static coalesce not allowed in adaptive RX mode");
		return -EINVAL;
	}

	if (!test_bit(HINIC3_INTF_UP, &nic_dev->flags) ||
	    q_id >= nic_dev->q_params.num_qps)
		return 0;

	spin_lock(&nic_dev->coal_lock);

	intr_coal = &nic_dev->intr_coalesce[q_id];

	intr_coal->coalesce_timer_cfg = coal->coalesce_timer_cfg;
	intr_coal->pending_limit = coal->pending_limit;
	intr_coal->rx_pending_limit_low = coal->rx_pending_limit_low;
	intr_coal->rx_pending_limit_high = coal->rx_pending_limit_high;
	spin_unlock(&nic_dev->coal_lock);

	info.msix_index = nic_dev->q_params.irq_cfg[q_id].msix_entry_idx;
	info.interrupt_coalesc_set = 1;
	info.coalesc_timer_cfg = intr_coal->coalesce_timer_cfg;
	info.pending_limit = intr_coal->pending_limit;
	info.resend_timer_cfg = intr_coal->resend_timer_cfg;
	err = hinic3_set_interrupt_cfg(nic_dev->hwdev, info);
	if (err) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "Failed to set queue%u coalesce",
				       q_id);
		return err;
	}

	return 0;
}

static int is_coalesce_exceed_limit(const struct ethtool_coalesce *coal,
				    struct netlink_ext_ack *extack)
{
	const struct {
		const char *name;
		u32 value;
		u32 limit;
	} coalesce_limits[] = {
		{"rx_coalesce_usecs",
		 coal->rx_coalesce_usecs,
		 COALESCE_MAX_TIMER_CFG},
		{"rx_max_coalesced_frames",
		 coal->rx_max_coalesced_frames,
		 COALESCE_MAX_PENDING_LIMIT},
		{"rx_max_coalesced_frames_low",
		 coal->rx_max_coalesced_frames_low,
		 COALESCE_MAX_PENDING_LIMIT},
		{"rx_max_coalesced_frames_high",
		 coal->rx_max_coalesced_frames_high,
		 COALESCE_MAX_PENDING_LIMIT},
	};

	for (int i = 0; i < ARRAY_SIZE(coalesce_limits); i++) {
		if (coalesce_limits[i].value > coalesce_limits[i].limit) {
			NL_SET_ERR_MSG_FMT_MOD(extack,
					       "%s out of range %d-%d",
					       coalesce_limits[i].name,
					       0,
					       coalesce_limits[i].limit);
			return -ERANGE;
		}
	}
	return 0;
}

static int is_coalesce_legal(const struct ethtool_coalesce *coal,
			     struct netlink_ext_ack *extack)
{
	int err;

	err = is_coalesce_exceed_limit(coal, extack);
	if (err)
		return err;

	if (coal->rx_max_coalesced_frames_low >
	    coal->rx_max_coalesced_frames_high) {
		NL_SET_ERR_MSG_FMT_MOD(extack,
				       "invalid coalesce frame high %u, low %u",
				       coal->rx_max_coalesced_frames_high,
				       coal->rx_max_coalesced_frames_low);
		return -ERANGE;
	}

	return 0;
}

static void check_coalesce_align(struct net_device *netdev,
				 u32 item, u32 unit, const char *str)
{
	if (item % unit)
		netdev_warn(netdev, "%s in %d units, change to %u\n",
			    str, unit, item - item % unit);
}

#define CHECK_COALESCE_ALIGN(member, unit) \
	check_coalesce_align(netdev, member, unit, #member)

static void check_coalesce_changed(struct net_device *netdev,
				   u32 item, u32 unit, u32 ori_val,
				   const char *obj_str, const char *str)
{
	if ((item / unit) != ori_val)
		netdev_dbg(netdev, "Change %s from %d to %u %s\n",
			   str, ori_val * unit, item - item % unit, obj_str);
}

#define CHECK_COALESCE_CHANGED(member, unit, ori_val, obj_str) \
	check_coalesce_changed(netdev, member, unit, ori_val, obj_str, #member)

static int hinic3_set_hw_coal_param(struct net_device *netdev,
				    struct hinic3_intr_coal_info *intr_coal,
				    struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	int err;
	u16 i;

	for (i = 0; i < nic_dev->max_qps; i++) {
		err = hinic3_set_queue_coalesce(netdev, i, intr_coal, extack);
		if (err)
			return err;
	}

	return 0;
}

static int hinic3_get_coalesce(struct net_device *netdev,
			       struct ethtool_coalesce *coal,
			       struct kernel_ethtool_coalesce *kernel_coal,
			       struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *interrupt_info;

	memset(coal, 0, sizeof(*coal));

	interrupt_info = &nic_dev->intr_coalesce[0];

	coal->use_adaptive_rx_coalesce = nic_dev->adaptive_rx_coal;

	if (nic_dev->adaptive_rx_coal) {
		coal->rx_max_coalesced_frames_low =
			interrupt_info->rx_pending_limit_low *
			COALESCE_PENDING_LIMIT_UNIT;
		coal->rx_max_coalesced_frames_high =
			interrupt_info->rx_pending_limit_high *
			COALESCE_PENDING_LIMIT_UNIT;
	} else {
		/* TX/RX uses the same interrupt.
		 * So we only declare RX ethtool_coalesce parameters.
		 */
		coal->rx_coalesce_usecs = interrupt_info->coalesce_timer_cfg *
					  COALESCE_TIMER_CFG_UNIT;
		coal->rx_max_coalesced_frames = interrupt_info->pending_limit *
						COALESCE_PENDING_LIMIT_UNIT;
	}

	return 0;
}

static int hinic3_set_coalesce(struct net_device *netdev,
			       struct ethtool_coalesce *coal,
			       struct kernel_ethtool_coalesce *kernel_coal,
			       struct netlink_ext_ack *extack)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);
	struct hinic3_intr_coal_info *ori_intr_coal;
	struct hinic3_intr_coal_info intr_coal = {};
	const char *obj_str = "for netdev";
	int err;

	err = is_coalesce_legal(coal, extack);
	if (err)
		return err;

	CHECK_COALESCE_ALIGN(coal->rx_coalesce_usecs, COALESCE_TIMER_CFG_UNIT);
	CHECK_COALESCE_ALIGN(coal->rx_max_coalesced_frames,
			     COALESCE_PENDING_LIMIT_UNIT);
	CHECK_COALESCE_ALIGN(coal->rx_max_coalesced_frames_high,
			     COALESCE_PENDING_LIMIT_UNIT);
	CHECK_COALESCE_ALIGN(coal->rx_max_coalesced_frames_low,
			     COALESCE_PENDING_LIMIT_UNIT);

	ori_intr_coal = &nic_dev->intr_coalesce[0];

	CHECK_COALESCE_CHANGED(coal->rx_coalesce_usecs, COALESCE_TIMER_CFG_UNIT,
			       ori_intr_coal->coalesce_timer_cfg, obj_str);
	CHECK_COALESCE_CHANGED(coal->rx_max_coalesced_frames,
			       COALESCE_PENDING_LIMIT_UNIT,
			       ori_intr_coal->pending_limit, obj_str);
	CHECK_COALESCE_CHANGED(coal->rx_max_coalesced_frames_high,
			       COALESCE_PENDING_LIMIT_UNIT,
			       ori_intr_coal->rx_pending_limit_high, obj_str);
	CHECK_COALESCE_CHANGED(coal->rx_max_coalesced_frames_low,
			       COALESCE_PENDING_LIMIT_UNIT,
			       ori_intr_coal->rx_pending_limit_low, obj_str);

	intr_coal.coalesce_timer_cfg =
		(u8)(coal->rx_coalesce_usecs / COALESCE_TIMER_CFG_UNIT);
	intr_coal.pending_limit = (u8)(coal->rx_max_coalesced_frames /
				      COALESCE_PENDING_LIMIT_UNIT);

	nic_dev->adaptive_rx_coal = coal->use_adaptive_rx_coalesce;

	intr_coal.rx_pending_limit_high =
		(u8)(coal->rx_max_coalesced_frames_high /
		     COALESCE_PENDING_LIMIT_UNIT);

	intr_coal.rx_pending_limit_low =
		(u8)(coal->rx_max_coalesced_frames_low /
		     COALESCE_PENDING_LIMIT_UNIT);

	/* coalesce timer or pending set to zero will disable coalesce */
	if (!nic_dev->adaptive_rx_coal &&
	    (!intr_coal.coalesce_timer_cfg || !intr_coal.pending_limit))
		NL_SET_ERR_MSG_MOD(extack, "Coalesce will be disabled");

	return hinic3_set_hw_coal_param(netdev, &intr_coal, extack);
}

static const struct ethtool_ops hinic3_ethtool_ops = {
	.supported_coalesce_params      = ETHTOOL_COALESCE_RX_USECS |
					  ETHTOOL_COALESCE_RX_MAX_FRAMES |
					  ETHTOOL_COALESCE_USE_ADAPTIVE_RX |
					  ETHTOOL_COALESCE_RX_MAX_FRAMES_LOW |
					  ETHTOOL_COALESCE_RX_MAX_FRAMES_HIGH,
	.get_link_ksettings             = hinic3_get_link_ksettings,
	.get_drvinfo                    = hinic3_get_drvinfo,
	.get_msglevel                   = hinic3_get_msglevel,
	.set_msglevel                   = hinic3_set_msglevel,
	.get_link                       = ethtool_op_get_link,
	.get_ringparam                  = hinic3_get_ringparam,
	.set_ringparam                  = hinic3_set_ringparam,
	.get_sset_count                 = hinic3_get_sset_count,
	.get_ethtool_stats              = hinic3_get_ethtool_stats,
	.get_strings                    = hinic3_get_strings,
	.get_eth_phy_stats              = hinic3_get_eth_phy_stats,
	.get_eth_mac_stats              = hinic3_get_eth_mac_stats,
	.get_eth_ctrl_stats             = hinic3_get_eth_ctrl_stats,
	.get_rmon_stats                 = hinic3_get_rmon_stats,
	.get_pause_stats                = hinic3_get_pause_stats,
	.get_coalesce                   = hinic3_get_coalesce,
	.set_coalesce                   = hinic3_set_coalesce,
};

void hinic3_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &hinic3_ethtool_ops;
}
