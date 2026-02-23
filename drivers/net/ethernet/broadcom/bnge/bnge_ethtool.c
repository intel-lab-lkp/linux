// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Broadcom.

#include <linux/unaligned.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <net/devlink.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/ethtool_netlink.h>

#include "bnge.h"
#include "bnge_ethtool.h"
#include "bnge_hwrm_lib.h"

static int bnge_nway_reset(struct net_device *dev)
{
	struct bnge_net *bn = netdev_priv(dev);
	struct bnge_dev *bd = bn->bd;
	int rc = 0;

	if (!BNGE_PHY_CFG_ABLE(bd))
		return -EOPNOTSUPP;

	if (!(bn->eth_link_info.autoneg & BNGE_AUTONEG_SPEED))
		return -EINVAL;

	if (netif_running(dev))
		rc = bnge_hwrm_set_link_setting(bn, true);

	return rc;
}

static const char * const bnge_ring_rx_stats_str[] = {
	"rx_ucast_packets",
	"rx_mcast_packets",
	"rx_bcast_packets",
	"rx_discards",
	"rx_errors",
	"rx_ucast_bytes",
	"rx_mcast_bytes",
	"rx_bcast_bytes",
};

static const char * const bnge_ring_tx_stats_str[] = {
	"tx_ucast_packets",
	"tx_mcast_packets",
	"tx_bcast_packets",
	"tx_errors",
	"tx_discards",
	"tx_ucast_bytes",
	"tx_mcast_bytes",
	"tx_bcast_bytes",
};

static const char * const bnge_ring_tpa_stats_str[] = {
	"tpa_packets",
	"tpa_bytes",
	"tpa_events",
	"tpa_aborts",
};

static const char * const bnge_ring_tpa2_stats_str[] = {
	"rx_tpa_eligible_pkt",
	"rx_tpa_eligible_bytes",
	"rx_tpa_pkt",
	"rx_tpa_bytes",
	"rx_tpa_errors",
	"rx_tpa_events",
};

static const char * const bnge_rx_sw_stats_str[] = {
	"rx_l4_csum_errors",
	"rx_resets",
	"rx_buf_errors",
};

#define BNGE_RX_STATS_ENTRY(counter)	\
	{ BNGE_RX_STATS_OFFSET(counter), __stringify(counter) }

#define BNGE_TX_STATS_ENTRY(counter)	\
	{ BNGE_TX_STATS_OFFSET(counter), __stringify(counter) }

#define BNGE_RX_STATS_EXT_ENTRY(counter)	\
	{ BNGE_RX_STATS_EXT_OFFSET(counter), __stringify(counter) }

#define BNGE_TX_STATS_EXT_ENTRY(counter)	\
	{ BNGE_TX_STATS_EXT_OFFSET(counter), __stringify(counter) }

#define BNGE_RX_STATS_EXT_PFC_ENTRY(n)				\
	BNGE_RX_STATS_EXT_ENTRY(pfc_pri##n##_rx_duration_us),	\
	BNGE_RX_STATS_EXT_ENTRY(pfc_pri##n##_rx_transitions)

#define BNGE_TX_STATS_EXT_PFC_ENTRY(n)				\
	BNGE_TX_STATS_EXT_ENTRY(pfc_pri##n##_tx_duration_us),	\
	BNGE_TX_STATS_EXT_ENTRY(pfc_pri##n##_tx_transitions)

#define BNGE_RX_STATS_EXT_PFC_ENTRIES				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(0),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(1),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(2),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(3),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(4),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(5),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(6),				\
	BNGE_RX_STATS_EXT_PFC_ENTRY(7)

#define BNGE_TX_STATS_EXT_PFC_ENTRIES				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(0),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(1),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(2),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(3),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(4),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(5),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(6),				\
	BNGE_TX_STATS_EXT_PFC_ENTRY(7)

#define BNGE_RX_STATS_EXT_COS_ENTRY(n)				\
	BNGE_RX_STATS_EXT_ENTRY(rx_bytes_cos##n),		\
	BNGE_RX_STATS_EXT_ENTRY(rx_packets_cos##n)

#define BNGE_TX_STATS_EXT_COS_ENTRY(n)				\
	BNGE_TX_STATS_EXT_ENTRY(tx_bytes_cos##n),		\
	BNGE_TX_STATS_EXT_ENTRY(tx_packets_cos##n)

#define BNGE_RX_STATS_EXT_COS_ENTRIES				\
	BNGE_RX_STATS_EXT_COS_ENTRY(0),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(1),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(2),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(3),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(4),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(5),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(6),				\
	BNGE_RX_STATS_EXT_COS_ENTRY(7)				\

#define BNGE_TX_STATS_EXT_COS_ENTRIES				\
	BNGE_TX_STATS_EXT_COS_ENTRY(0),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(1),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(2),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(3),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(4),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(5),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(6),				\
	BNGE_TX_STATS_EXT_COS_ENTRY(7)				\

#define BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(n)			\
	BNGE_RX_STATS_EXT_ENTRY(rx_discard_bytes_cos##n),	\
	BNGE_RX_STATS_EXT_ENTRY(rx_discard_packets_cos##n)

#define BNGE_RX_STATS_EXT_DISCARD_COS_ENTRIES				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(0),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(1),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(2),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(3),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(4),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(5),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(6),				\
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRY(7)

#define BNGE_RX_STATS_PRI_ENTRY(counter, n)		\
	{ BNGE_RX_STATS_EXT_OFFSET(counter##_cos0),	\
	  __stringify(counter##_pri##n) }

#define BNGE_TX_STATS_PRI_ENTRY(counter, n)		\
	{ BNGE_TX_STATS_EXT_OFFSET(counter##_cos0),	\
	  __stringify(counter##_pri##n) }

#define BNGE_RX_STATS_PRI_ENTRIES(counter)		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 0),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 1),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 2),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 3),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 4),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 5),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 6),		\
	BNGE_RX_STATS_PRI_ENTRY(counter, 7)

#define BNGE_TX_STATS_PRI_ENTRIES(counter)		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 0),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 1),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 2),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 3),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 4),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 5),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 6),		\
	BNGE_TX_STATS_PRI_ENTRY(counter, 7)

static const char *const bnge_ring_err_stats_arr[] = {
	"rx_total_l4_csum_errors",
	"rx_total_resets",
	"rx_total_buf_errors",
	"rx_total_oom_discards",
	"rx_total_netpoll_discards",
	"rx_total_ring_discards",
	"tx_total_resets",
	"tx_total_ring_discards",
};

#define NUM_RING_RX_SW_STATS		ARRAY_SIZE(bnge_rx_sw_stats_str)
#define NUM_RING_RX_HW_STATS		ARRAY_SIZE(bnge_ring_rx_stats_str)
#define NUM_RING_TX_HW_STATS		ARRAY_SIZE(bnge_ring_tx_stats_str)

static const struct {
	long offset;
	char string[ETH_GSTRING_LEN];
} bnge_tx_port_stats_ext_arr[] = {
	BNGE_TX_STATS_EXT_COS_ENTRIES,
	BNGE_TX_STATS_EXT_PFC_ENTRIES,
};

static const struct {
	long base_off;
	char string[ETH_GSTRING_LEN];
} bnge_rx_bytes_pri_arr[] = {
	BNGE_RX_STATS_PRI_ENTRIES(rx_bytes),
};

static const struct {
	long base_off;
	char string[ETH_GSTRING_LEN];
} bnge_rx_pkts_pri_arr[] = {
	BNGE_RX_STATS_PRI_ENTRIES(rx_packets),
};

static const struct {
	long base_off;
	char string[ETH_GSTRING_LEN];
} bnge_tx_bytes_pri_arr[] = {
	BNGE_TX_STATS_PRI_ENTRIES(tx_bytes),
};

static const struct {
	long base_off;
	char string[ETH_GSTRING_LEN];
} bnge_tx_pkts_pri_arr[] = {
	BNGE_TX_STATS_PRI_ENTRIES(tx_packets),
};

static const struct {
	long offset;
	char string[ETH_GSTRING_LEN];
} bnge_port_stats_arr[] = {
	BNGE_RX_STATS_ENTRY(rx_64b_frames),
	BNGE_RX_STATS_ENTRY(rx_65b_127b_frames),
	BNGE_RX_STATS_ENTRY(rx_128b_255b_frames),
	BNGE_RX_STATS_ENTRY(rx_256b_511b_frames),
	BNGE_RX_STATS_ENTRY(rx_512b_1023b_frames),
	BNGE_RX_STATS_ENTRY(rx_1024b_1518b_frames),
	BNGE_RX_STATS_ENTRY(rx_good_vlan_frames),
	BNGE_RX_STATS_ENTRY(rx_1519b_2047b_frames),
	BNGE_RX_STATS_ENTRY(rx_2048b_4095b_frames),
	BNGE_RX_STATS_ENTRY(rx_4096b_9216b_frames),
	BNGE_RX_STATS_ENTRY(rx_9217b_16383b_frames),
	BNGE_RX_STATS_ENTRY(rx_total_frames),
	BNGE_RX_STATS_ENTRY(rx_ucast_frames),
	BNGE_RX_STATS_ENTRY(rx_mcast_frames),
	BNGE_RX_STATS_ENTRY(rx_bcast_frames),
	BNGE_RX_STATS_ENTRY(rx_fcs_err_frames),
	BNGE_RX_STATS_ENTRY(rx_ctrl_frames),
	BNGE_RX_STATS_ENTRY(rx_pause_frames),
	BNGE_RX_STATS_ENTRY(rx_pfc_frames),
	BNGE_RX_STATS_ENTRY(rx_align_err_frames),
	BNGE_RX_STATS_ENTRY(rx_ovrsz_frames),
	BNGE_RX_STATS_ENTRY(rx_jbr_frames),
	BNGE_RX_STATS_ENTRY(rx_mtu_err_frames),
	BNGE_RX_STATS_ENTRY(rx_tagged_frames),
	BNGE_RX_STATS_ENTRY(rx_double_tagged_frames),
	BNGE_RX_STATS_ENTRY(rx_good_frames),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri0),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri1),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri2),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri3),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri4),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri5),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri6),
	BNGE_RX_STATS_ENTRY(rx_pfc_ena_frames_pri7),
	BNGE_RX_STATS_ENTRY(rx_undrsz_frames),
	BNGE_RX_STATS_ENTRY(rx_eee_lpi_events),
	BNGE_RX_STATS_ENTRY(rx_eee_lpi_duration),
	BNGE_RX_STATS_ENTRY(rx_bytes),
	BNGE_RX_STATS_ENTRY(rx_runt_bytes),
	BNGE_RX_STATS_ENTRY(rx_runt_frames),
	BNGE_RX_STATS_ENTRY(rx_stat_discard),
	BNGE_RX_STATS_ENTRY(rx_stat_err),

	BNGE_TX_STATS_ENTRY(tx_64b_frames),
	BNGE_TX_STATS_ENTRY(tx_65b_127b_frames),
	BNGE_TX_STATS_ENTRY(tx_128b_255b_frames),
	BNGE_TX_STATS_ENTRY(tx_256b_511b_frames),
	BNGE_TX_STATS_ENTRY(tx_512b_1023b_frames),
	BNGE_TX_STATS_ENTRY(tx_1024b_1518b_frames),
	BNGE_TX_STATS_ENTRY(tx_good_vlan_frames),
	BNGE_TX_STATS_ENTRY(tx_1519b_2047b_frames),
	BNGE_TX_STATS_ENTRY(tx_2048b_4095b_frames),
	BNGE_TX_STATS_ENTRY(tx_4096b_9216b_frames),
	BNGE_TX_STATS_ENTRY(tx_9217b_16383b_frames),
	BNGE_TX_STATS_ENTRY(tx_good_frames),
	BNGE_TX_STATS_ENTRY(tx_total_frames),
	BNGE_TX_STATS_ENTRY(tx_ucast_frames),
	BNGE_TX_STATS_ENTRY(tx_mcast_frames),
	BNGE_TX_STATS_ENTRY(tx_bcast_frames),
	BNGE_TX_STATS_ENTRY(tx_pause_frames),
	BNGE_TX_STATS_ENTRY(tx_pfc_frames),
	BNGE_TX_STATS_ENTRY(tx_jabber_frames),
	BNGE_TX_STATS_ENTRY(tx_fcs_err_frames),
	BNGE_TX_STATS_ENTRY(tx_err),
	BNGE_TX_STATS_ENTRY(tx_fifo_underruns),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri0),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri1),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri2),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri3),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri4),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri5),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri6),
	BNGE_TX_STATS_ENTRY(tx_pfc_ena_frames_pri7),
	BNGE_TX_STATS_ENTRY(tx_eee_lpi_events),
	BNGE_TX_STATS_ENTRY(tx_eee_lpi_duration),
	BNGE_TX_STATS_ENTRY(tx_total_collisions),
	BNGE_TX_STATS_ENTRY(tx_bytes),
	BNGE_TX_STATS_ENTRY(tx_xthol_frames),
	BNGE_TX_STATS_ENTRY(tx_stat_discard),
	BNGE_TX_STATS_ENTRY(tx_stat_error),
};

static const struct {
	long offset;
	char string[ETH_GSTRING_LEN];
} bnge_port_stats_ext_arr[] = {
	BNGE_RX_STATS_EXT_ENTRY(link_down_events),
	BNGE_RX_STATS_EXT_ENTRY(continuous_pause_events),
	BNGE_RX_STATS_EXT_ENTRY(resume_pause_events),
	BNGE_RX_STATS_EXT_ENTRY(continuous_roce_pause_events),
	BNGE_RX_STATS_EXT_ENTRY(resume_roce_pause_events),
	BNGE_RX_STATS_EXT_COS_ENTRIES,
	BNGE_RX_STATS_EXT_PFC_ENTRIES,
	BNGE_RX_STATS_EXT_ENTRY(rx_bits),
	BNGE_RX_STATS_EXT_ENTRY(rx_buffer_passed_threshold),
	BNGE_RX_STATS_EXT_ENTRY(rx_pcs_symbol_err),
	BNGE_RX_STATS_EXT_ENTRY(rx_corrected_bits),
	BNGE_RX_STATS_EXT_DISCARD_COS_ENTRIES,
	BNGE_RX_STATS_EXT_ENTRY(rx_fec_corrected_blocks),
	BNGE_RX_STATS_EXT_ENTRY(rx_fec_uncorrectable_blocks),
	BNGE_RX_STATS_EXT_ENTRY(rx_filter_miss),
};

static int bnge_get_num_tpa_ring_stats(struct bnge_dev *bd)
{
	if (BNGE_SUPPORTS_TPA(bd))
		return BNGE_NUM_TPA_RING_STATS;
	return 0;
}

#define BNGE_NUM_RING_ERR_STATS	ARRAY_SIZE(bnge_ring_err_stats_arr)
#define BNGE_NUM_PORT_STATS ARRAY_SIZE(bnge_port_stats_arr)
#define BNGE_NUM_STATS_PRI			\
	(ARRAY_SIZE(bnge_rx_bytes_pri_arr) +	\
	 ARRAY_SIZE(bnge_rx_pkts_pri_arr) +	\
	 ARRAY_SIZE(bnge_tx_bytes_pri_arr) +	\
	 ARRAY_SIZE(bnge_tx_pkts_pri_arr))

static int bnge_get_num_ring_stats(struct bnge_dev *bd)
{
	int rx, tx;

	rx = NUM_RING_RX_HW_STATS + NUM_RING_RX_SW_STATS +
	     bnge_get_num_tpa_ring_stats(bd);
	tx = NUM_RING_TX_HW_STATS;
	return rx * bd->rx_nr_rings +
	       tx * bd->tx_nr_rings_per_tc;
}

static int bnge_get_num_stats(struct bnge_net *bn)
{
	int num_stats = bnge_get_num_ring_stats(bn->bd);
	int len;

	num_stats += BNGE_NUM_RING_ERR_STATS;

	if (bn->flags & BNGE_FLAG_PORT_STATS)
		num_stats += BNGE_NUM_PORT_STATS;

	if (bn->flags & BNGE_FLAG_PORT_STATS_EXT) {
		len = min_t(int, bn->fw_rx_stats_ext_size,
			    ARRAY_SIZE(bnge_port_stats_ext_arr));
		num_stats += len;
		len = min_t(int, bn->fw_tx_stats_ext_size,
			    ARRAY_SIZE(bnge_tx_port_stats_ext_arr));
		num_stats += len;
		if (bn->pri2cos_valid)
			num_stats += BNGE_NUM_STATS_PRI;
	}

	return num_stats;
}

static void bnge_get_drvinfo(struct net_device *dev,
			     struct ethtool_drvinfo *info)
{
	struct bnge_net *bn = netdev_priv(dev);
	struct bnge_dev *bd = bn->bd;

	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->fw_version, bd->fw_ver_str, sizeof(info->fw_version));
	strscpy(info->bus_info, pci_name(bd->pdev), sizeof(info->bus_info));
	info->n_stats = bnge_get_num_stats(bn);
}

static int bnge_get_sset_count(struct net_device *dev, int sset)
{
	struct bnge_net *bn = netdev_priv(dev);

	switch (sset) {
	case ETH_SS_STATS:
		return bnge_get_num_stats(bn);
	default:
		return -EOPNOTSUPP;
	}
}

static bool is_rx_ring(struct bnge_dev *bd, u16 ring_num)
{
	return ring_num < bd->rx_nr_rings;
}

static bool is_tx_ring(struct bnge_dev *bd, u16 ring_num)
{
	u16 tx_base = 0;

	if (!(bd->flags & BNGE_EN_SHARED_CHNL))
		tx_base = bd->rx_nr_rings;

	if (ring_num >= tx_base && ring_num < (tx_base + bd->tx_nr_rings))
		return true;
	return false;
}

static void bnge_get_ethtool_stats(struct net_device *dev,
				   struct ethtool_stats *stats, u64 *buf)
{
	struct bnge_total_ring_err_stats ring_err_stats = {};
	struct bnge_net *bn = netdev_priv(dev);
	struct bnge_dev *bd = bn->bd;
	u64 *curr, *prev;
	u32 tpa_stats;
	u32 i, j = 0;

	if (!bn->bnapi) {
		j += bnge_get_num_ring_stats(bd);
		goto skip_ring_stats;
	}

	tpa_stats = bnge_get_num_tpa_ring_stats(bd);
	for (i = 0; i < bd->nq_nr_rings; i++) {
		struct bnge_napi *bnapi = bn->bnapi[i];
		struct bnge_nq_ring_info *nqr = &bnapi->nq_ring;
		u64 *sw_stats = nqr->stats.sw_stats;
		u64 *sw;
		int k;

		if (is_rx_ring(bd, i)) {
			for (k = 0; k < NUM_RING_RX_HW_STATS; j++, k++)
				buf[j] = sw_stats[k];
		}
		if (is_tx_ring(bd, i)) {
			k = NUM_RING_RX_HW_STATS;
			for (; k < NUM_RING_RX_HW_STATS + NUM_RING_TX_HW_STATS;
			       j++, k++)
				buf[j] = sw_stats[k];
		}
		if (!tpa_stats || !is_rx_ring(bd, i))
			goto skip_tpa_ring_stats;

		k = NUM_RING_RX_HW_STATS + NUM_RING_TX_HW_STATS;
		for (; k < NUM_RING_RX_HW_STATS + NUM_RING_TX_HW_STATS +
			   tpa_stats; j++, k++)
			buf[j] = sw_stats[k];

skip_tpa_ring_stats:
		sw = (u64 *)&nqr->sw_stats->rx;
		if (is_rx_ring(bd, i)) {
			for (k = 0; k < NUM_RING_RX_SW_STATS; j++, k++)
				buf[j] = sw[k];
		}
	}

	bnge_get_ring_err_stats(bn, &ring_err_stats);

skip_ring_stats:
	curr = &ring_err_stats.rx_total_l4_csum_errors;
	prev = &bn->ring_err_stats_prev.rx_total_l4_csum_errors;
	for (i = 0; i < BNGE_NUM_RING_ERR_STATS; i++, j++, curr++, prev++)
		buf[j] = *curr + *prev;

	if (bn->flags & BNGE_FLAG_PORT_STATS) {
		u64 *port_stats = bn->port_stats.sw_stats;

		for (i = 0; i < BNGE_NUM_PORT_STATS; i++, j++)
			buf[j] = *(port_stats + bnge_port_stats_arr[i].offset);
	}
	if (bn->flags & BNGE_FLAG_PORT_STATS_EXT) {
		u64 *rx_port_stats_ext = bn->rx_port_stats_ext.sw_stats;
		u64 *tx_port_stats_ext = bn->tx_port_stats_ext.sw_stats;
		u32 len;

		len = min_t(u32, bn->fw_rx_stats_ext_size,
			    ARRAY_SIZE(bnge_port_stats_ext_arr));
		for (i = 0; i < len; i++, j++) {
			buf[j] = *(rx_port_stats_ext +
				   bnge_port_stats_ext_arr[i].offset);
		}
		len = min_t(u32, bn->fw_tx_stats_ext_size,
			    ARRAY_SIZE(bnge_tx_port_stats_ext_arr));
		for (i = 0; i < len; i++, j++) {
			buf[j] = *(tx_port_stats_ext +
				   bnge_tx_port_stats_ext_arr[i].offset);
		}
		if (bn->pri2cos_valid) {
			for (i = 0; i < 8; i++, j++) {
				long n = bnge_rx_bytes_pri_arr[i].base_off +
					 bn->pri2cos_idx[i];

				buf[j] = *(rx_port_stats_ext + n);
			}
			for (i = 0; i < 8; i++, j++) {
				long n = bnge_rx_pkts_pri_arr[i].base_off +
					 bn->pri2cos_idx[i];

				buf[j] = *(rx_port_stats_ext + n);
			}
			for (i = 0; i < 8; i++, j++) {
				long n = bnge_tx_bytes_pri_arr[i].base_off +
					 bn->pri2cos_idx[i];

				buf[j] = *(tx_port_stats_ext + n);
			}
			for (i = 0; i < 8; i++, j++) {
				long n = bnge_tx_pkts_pri_arr[i].base_off +
					 bn->pri2cos_idx[i];

				buf[j] = *(tx_port_stats_ext + n);
			}
		}
	}
}

static void bnge_get_strings(struct net_device *dev, u32 stringset, u8 *buf)
{
	struct bnge_net *bn = netdev_priv(dev);
	struct bnge_dev *bd = bn->bd;
	u32 i, j, num_str;
	const char *str;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < bd->nq_nr_rings; i++) {
			if (is_rx_ring(bd, i))
				for (j = 0; j < NUM_RING_RX_HW_STATS; j++) {
					str = bnge_ring_rx_stats_str[j];
					ethtool_sprintf(&buf, "[%d]: %s", i,
							str);
				}
			if (is_tx_ring(bd, i))
				for (j = 0; j < NUM_RING_TX_HW_STATS; j++) {
					str = bnge_ring_tx_stats_str[j];
					ethtool_sprintf(&buf, "[%d]: %s", i,
							str);
				}
			num_str = bnge_get_num_tpa_ring_stats(bd);
			if (!num_str || !is_rx_ring(bd, i))
				goto skip_tpa_stats;

			if (bd->max_tpa_v2)
				for (j = 0; j < num_str; j++) {
					str = bnge_ring_tpa2_stats_str[j];
					ethtool_sprintf(&buf, "[%d]: %s", i,
							str);
				}
			else
				for (j = 0; j < num_str; j++) {
					str = bnge_ring_tpa_stats_str[j];
					ethtool_sprintf(&buf, "[%d]: %s", i,
							str);
				}
skip_tpa_stats:
			if (is_rx_ring(bd, i))
				for (j = 0; j < NUM_RING_RX_SW_STATS; j++) {
					str = bnge_rx_sw_stats_str[j];
					ethtool_sprintf(&buf, "[%d]: %s", i,
							str);
				}
		}
		for (i = 0; i < BNGE_NUM_RING_ERR_STATS; i++)
			ethtool_puts(&buf, bnge_ring_err_stats_arr[i]);

		if (bn->flags & BNGE_FLAG_PORT_STATS)
			for (i = 0; i < BNGE_NUM_PORT_STATS; i++) {
				str = bnge_port_stats_arr[i].string;
				ethtool_puts(&buf, str);
			}

		if (bn->flags & BNGE_FLAG_PORT_STATS_EXT) {
			u32 len;

			len = min_t(u32, bn->fw_rx_stats_ext_size,
				    ARRAY_SIZE(bnge_port_stats_ext_arr));
			for (i = 0; i < len; i++) {
				str = bnge_port_stats_ext_arr[i].string;
				ethtool_puts(&buf, str);
			}

			len = min_t(u32, bn->fw_tx_stats_ext_size,
				    ARRAY_SIZE(bnge_tx_port_stats_ext_arr));
			for (i = 0; i < len; i++) {
				str = bnge_tx_port_stats_ext_arr[i].string;
				ethtool_puts(&buf, str);
			}

			if (bn->pri2cos_valid) {
				for (i = 0; i < 8; i++) {
					str = bnge_rx_bytes_pri_arr[i].string;
					ethtool_puts(&buf, str);
				}

				for (i = 0; i < 8; i++) {
					str = bnge_rx_pkts_pri_arr[i].string;
					ethtool_puts(&buf, str);
				}

				for (i = 0; i < 8; i++) {
					str = bnge_tx_bytes_pri_arr[i].string;
					ethtool_puts(&buf, str);
				}

				for (i = 0; i < 8; i++) {
					str = bnge_tx_pkts_pri_arr[i].string;
					ethtool_puts(&buf, str);
				}
			}
		}
		break;
	default:
		netdev_err(bd->netdev, "%s invalid request %x\n",
			   __func__, stringset);
		break;
	}
}

static const struct ethtool_ops bnge_ethtool_ops = {
	.cap_link_lanes_supported	= 1,
	.get_link_ksettings	= bnge_get_link_ksettings,
	.set_link_ksettings	= bnge_set_link_ksettings,
	.get_drvinfo		= bnge_get_drvinfo,
	.get_link		= bnge_get_link,
	.nway_reset		= bnge_nway_reset,
	.get_sset_count		= bnge_get_sset_count,
	.get_strings		= bnge_get_strings,
	.get_ethtool_stats	= bnge_get_ethtool_stats,
};

void bnge_set_ethtool_ops(struct net_device *dev)
{
	dev->ethtool_ops = &bnge_ethtool_ops;
}
