// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/ethtool.h>
#include <linux/phy.h>
#include "hbg_common.h"
#include "hbg_ethtool.h"
#include "hbg_hw.h"

#define HBG_STATS_FIELD_OFF(f) (offsetof(struct hbg_stats, f))
#define HBG_STATS_READ(p, offset) (*(u64 *)((u8 *)(p) + (offset)))
#define HBG_STATS_UPDATE(p, offset, val) (HBG_STATS_READ(p, offset) += (val))

struct hbg_ethtool_stats {
	char name[ETH_GSTRING_LEN];
	unsigned long offset;
	u32 reg; /* set to 0 if stats is not updated via dump reg */
};

#define HBG_STATS_I(stats) { #stats, HBG_STATS_FIELD_OFF(stats), 0}
#define HBG_STATS_REG_I(stats, reg) { #stats, HBG_STATS_FIELD_OFF(stats), reg}

static const struct hbg_ethtool_stats hbg_ethtool_stats_map[] = {
	HBG_STATS_I(rx_desc_l2_err_cnt),
	HBG_STATS_I(rx_desc_pkt_len_err_cnt),
	HBG_STATS_I(rx_desc_l3_err_cnt),
	HBG_STATS_I(rx_desc_l3_wrong_head_cnt),
	HBG_STATS_I(rx_desc_l3_csum_err_cnt),
	HBG_STATS_I(rx_desc_l3_len_err_cnt),
	HBG_STATS_I(rx_desc_l3_zero_ttl_cnt),
	HBG_STATS_I(rx_desc_l3_other_cnt),
	HBG_STATS_I(rx_desc_l4_err_cnt),
	HBG_STATS_I(rx_desc_l4_wrong_head_cnt),
	HBG_STATS_I(rx_desc_l4_len_err_cnt),
	HBG_STATS_I(rx_desc_l4_csum_err_cnt),
	HBG_STATS_I(rx_desc_l4_zero_port_num_cnt),
	HBG_STATS_I(rx_desc_l4_other_cnt),
	HBG_STATS_I(rx_desc_frag_cnt),
	HBG_STATS_I(rx_desc_ip_ver_err_cnt),
	HBG_STATS_I(rx_desc_ipv4_pkt_cnt),
	HBG_STATS_I(rx_desc_ipv6_pkt_cnt),
	HBG_STATS_I(rx_desc_no_ip_pkt_cnt),
	HBG_STATS_I(rx_desc_ip_pkt_cnt),
	HBG_STATS_I(rx_desc_tcp_pkt_cnt),
	HBG_STATS_I(rx_desc_udp_pkt_cnt),
	HBG_STATS_I(rx_desc_vlan_pkt_cnt),
	HBG_STATS_I(rx_desc_icmp_pkt_cnt),
	HBG_STATS_I(rx_desc_arp_pkt_cnt),
	HBG_STATS_I(rx_desc_rarp_pkt_cnt),
	HBG_STATS_I(rx_desc_multicast_pkt_cnt),
	HBG_STATS_I(rx_desc_broadcast_pkt_cnt),
	HBG_STATS_I(rx_desc_ipsec_pkt_cnt),
	HBG_STATS_I(rx_desc_ip_opt_pkt_cnt),
	HBG_STATS_I(rx_desc_key_not_match_cnt),

	HBG_STATS_REG_I(rx_octets_total_ok_cnt,
			HBG_REG_RX_OCTETS_TOTAL_OK_ADDR),
	HBG_STATS_REG_I(rx_octets_bad_cnt, HBG_REG_RX_OCTETS_BAD_ADDR),
	HBG_STATS_REG_I(rx_octets_total_filt_cnt,
			HBG_REG_RX_OCTETS_TOTAL_FILT_ADDR),
	HBG_STATS_REG_I(rx_uc_pkts_cnt, HBG_REG_RX_UC_PKTS_ADDR),
	HBG_STATS_REG_I(rx_mc_pkts_cnt, HBG_REG_RX_MC_PKTS_ADDR),
	HBG_STATS_REG_I(rx_bc_pkts_cnt, HBG_REG_RX_BC_PKTS_ADDR),
	HBG_STATS_REG_I(rx_vlan_pkt_cnt, HBG_REG_RX_TAGGED_ADDR),
	HBG_STATS_REG_I(rx_filt_pkt_cnt, HBG_REG_RX_FILT_PKT_CNT_ADDR),
	HBG_STATS_REG_I(rx_trans_pkg_cnt, HBG_REG_RX_TRANS_PKG_CNT_ADDR),
	HBG_STATS_REG_I(rx_framesize_64, HBG_REG_RX_PKTS_64OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_65_127,
			HBG_REG_RX_PKTS_65TO127OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_128_255,
			HBG_REG_RX_PKTS_128TO255OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_256_511,
			HBG_REG_RX_PKTS_256TO511OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_512_1023,
			HBG_REG_RX_PKTS_512TO1023OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_1024_1518,
			HBG_REG_RX_PKTS_1024TO1518OCTETS_ADDR),
	HBG_STATS_REG_I(rx_framesize_bt_1518,
			HBG_REG_RX_PKTS_1519TOMAXOCTETS_ADDR),
	HBG_STATS_REG_I(rx_fcs_error_cnt, HBG_REG_RX_FCS_ERRORS_ADDR),
	HBG_STATS_REG_I(rx_data_error_cnt, HBG_REG_RX_DATA_ERR_ADDR),
	HBG_STATS_REG_I(rx_align_error_cnt, HBG_REG_RX_ALIGN_ERRORS_ADDR),
	HBG_STATS_REG_I(rx_frame_long_err_cnt, HBG_REG_RX_LONG_ERRORS_ADDR),
	HBG_STATS_REG_I(rx_jabber_err_cnt, HBG_REG_RX_JABBER_ERRORS_ADDR),
	HBG_STATS_REG_I(rx_pause_macctl_frame_cnt,
			HBG_REG_RX_PAUSE_MACCTL_FRAMCOUNTER_ADDR),
	HBG_STATS_REG_I(rx_unknown_macctl_frame_cnt,
			HBG_REG_RX_UNKNOWN_MACCTL_FRAMCOUNTER_ADDR),
	HBG_STATS_REG_I(rx_frame_very_long_err_cnt,
			HBG_REG_RX_VERY_LONG_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_frame_runt_err_cnt, HBG_REG_RX_RUNT_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_frame_short_err_cnt, HBG_REG_RX_SHORT_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_over_flow_cnt, HBG_REG_RX_OVER_FLOW_CNT_ADDR),
	HBG_STATS_REG_I(rx_addr_overflow_cnt, HBG_REG_RX_ADDR_OVERFLOW_ADDR),
	HBG_STATS_REG_I(rx_bufrq_err_cnt, HBG_REG_RX_BUFRQ_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_we_err_cnt, HBG_REG_RX_WE_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_overrun_cnt, HBG_REG_RX_OVERRUN_CNT_ADDR),
	HBG_STATS_REG_I(rx_lengthfield_err_cnt,
			HBG_REG_RX_LENGTHFIELD_ERR_CNT_ADDR),
	HBG_STATS_REG_I(rx_fail_comma_cnt, HBG_REG_RX_FAIL_COMMA_CNT_ADDR),
	HBG_STATS_I(rx_dma_err_cnt),
	HBG_STATS_I(rx_fifo_fill_full_cnt),

	HBG_STATS_REG_I(tx_octets_total_ok_cnt,
			HBG_REG_OCTETS_TRANSMITTED_OK_ADDR),
	HBG_STATS_REG_I(tx_uc_pkts_cnt, HBG_REG_TX_UC_PKTS_ADDR),
	HBG_STATS_REG_I(tx_mc_pkts_cnt, HBG_REG_TX_MC_PKTS_ADDR),
	HBG_STATS_REG_I(tx_bc_pkts_cnt, HBG_REG_TX_BC_PKTS_ADDR),
	HBG_STATS_REG_I(tx_vlan_pkt_cnt, HBG_REG_TX_TAGGED_ADDR),
	HBG_STATS_REG_I(tx_octets_bad_cnt, HBG_REG_OCTETS_TRANSMITTED_BAD_ADDR),
	HBG_STATS_REG_I(tx_trans_pkg_cnt, HBG_REG_TX_TRANS_PKG_CNT_ADDR),
	HBG_STATS_REG_I(tx_pause_frame_cnt, HBG_REG_TX_PAUSE_FRAMES_ADDR),
	HBG_STATS_REG_I(tx_framesize_64, HBG_REG_TX_PKTS_64OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_65_127,
			HBG_REG_TX_PKTS_65TO127OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_128_255,
			HBG_REG_TX_PKTS_128TO255OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_256_511,
			HBG_REG_TX_PKTS_256TO511OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_512_1023,
			HBG_REG_TX_PKTS_512TO1023OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_1024_1518,
			HBG_REG_TX_PKTS_1024TO1518OCTETS_ADDR),
	HBG_STATS_REG_I(tx_framesize_bt_1518,
			HBG_REG_TX_PKTS_1519TOMAXOCTETS_ADDR),
	HBG_STATS_REG_I(tx_underrun_err_cnt, HBG_REG_TX_UNDERRUN_ADDR),
	HBG_STATS_REG_I(tx_add_cs_fail_cnt, HBG_REG_TX_CS_FAIL_CNT_ADDR),
	HBG_STATS_REG_I(tx_bufrl_err_cnt, HBG_REG_TX_BUFRL_ERR_CNT_ADDR),
	HBG_STATS_REG_I(tx_crc_err_cnt, HBG_REG_TX_CRC_ERROR_ADDR),
	HBG_STATS_REG_I(tx_drop_cnt, HBG_REG_TX_DROP_CNT_ADDR),
	HBG_STATS_REG_I(tx_excessive_length_drop_cnt,
			HBG_REG_TX_EXCESSIVE_LENGTH_DROP_ADDR),
	HBG_STATS_I(tx_dma_err_cnt),
};

static int hbg_ethtool_get_sset_count(struct net_device *netdev, int stringset)
{
	if (stringset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return ARRAY_SIZE(hbg_ethtool_stats_map);
}

static void hbg_ethtool_get_strings(struct net_device *netdev,
				    u32 stringset, u8 *data)
{
	u32 i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(hbg_ethtool_stats_map); i++)
		ethtool_puts(&data, hbg_ethtool_stats_map[i].name);
}

void hbg_update_stats(struct hbg_priv *priv)
{
	const struct hbg_ethtool_stats *stats_info;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(hbg_ethtool_stats_map); i++) {
		stats_info = &hbg_ethtool_stats_map[i];
		if (!stats_info->reg)
			continue;

		HBG_STATS_UPDATE(&priv->stats, stats_info->offset,
				 hbg_reg_read(priv, stats_info->reg));
	}
}

static void hbg_ethtool_get_stats(struct net_device *netdev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct hbg_priv *priv = netdev_priv(netdev);
	u32 i;

	hbg_update_stats(priv);
	for (i = 0; i < ARRAY_SIZE(hbg_ethtool_stats_map); i++)
		*data++ = HBG_STATS_READ(&priv->stats,
					 hbg_ethtool_stats_map[i].offset);
}

static const struct ethtool_ops hbg_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_sset_count		= hbg_ethtool_get_sset_count,
	.get_strings		= hbg_ethtool_get_strings,
	.get_ethtool_stats	= hbg_ethtool_get_stats,
};

void hbg_ethtool_set_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &hbg_ethtool_ops;
}
