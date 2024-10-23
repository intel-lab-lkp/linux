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

#define HBG_REG_NAEM_MAX_LEN	24
#define HBG_REG_TYPE_MAX_LEN	8

struct hbg_reg_offset_name_map {
	u32 reg_offset;
	char name[HBG_REG_NAEM_MAX_LEN];
};

struct hbg_reg_type_info {
	char name[HBG_REG_TYPE_MAX_LEN];
	u32 offset_base;
	const struct hbg_reg_offset_name_map *reg_maps;
	u32 reg_num;
};

struct hbg_reg_info {
	char name[HBG_REG_NAEM_MAX_LEN + HBG_REG_TYPE_MAX_LEN];
	u32 offset;
	u32 val;
};

const struct hbg_reg_offset_name_map hbg_dev_spec_reg_map[] = {
	{HBG_REG_SPEC_VALID_ADDR, "VALID"},
	{HBG_REG_EVENT_REQ_ADDR, "EVENT_REQ"},
	{HBG_REG_MAC_ID_ADDR, "MAC_ID"},
	{HBG_REG_PHY_ID_ADDR, "PHY_ADDR"},
	{HBG_REG_MAC_ADDR_ADDR, "MAC_ADDR_L"},
	{HBG_REG_MAC_ADDR_HIGH_ADDR, "MAC_ADDR_H"},
	{HBG_REG_UC_MAC_NUM_ADDR, "UC_MAX_NUM"},
	{HBG_REG_MAX_MTU_ADDR, "MAX_MTU"},
	{HBG_REG_MIN_MTU_ADDR, "MIN_MTU"},
	{HBG_REG_TX_FIFO_NUM_ADDR, "TX_FIFO_NUM"},
	{HBG_REG_RX_FIFO_NUM_ADDR, "RX_FIFO_NUM"},
	{HBG_REG_VLAN_LAYERS_ADDR, "VLAN_LAYERS"},
};

const struct hbg_reg_offset_name_map hbg_mdio_reg_map[] = {
	{HBG_REG_MDIO_COMMAND_ADDR, "COMMAND_REG"},
	{HBG_REG_MDIO_ADDR_ADDR, "ADDR_REG"},
	{HBG_REG_MDIO_WDATA_ADDR, "WDATA_REG"},
	{HBG_REG_MDIO_RDATA_ADDR, "RDATA_REG"},
	{HBG_REG_MDIO_STA_ADDR, "STA_REG"},
};

const struct hbg_reg_offset_name_map hbg_gmac_reg_map[] = {
	{HBG_REG_DUPLEX_TYPE_ADDR, "DUPLEX_TYPE"},
	{HBG_REG_FD_FC_TYPE_ADDR, "FD_FC_TYPE"},
	{HBG_REG_FC_TX_TIMER_ADDR, "FC_TX_TIMER"},
	{HBG_REG_FD_FC_ADDR_LOW_ADDR, "FD_FC_ADDR_LOW"},
	{HBG_REG_FD_FC_ADDR_HIGH_ADDR, "FD_FC_ADDR_HIGH"},
	{HBG_REG_MAX_FRAME_SIZE_ADDR, "MAX_FRM_SIZE"},
	{HBG_REG_PORT_MODE_ADDR, "PORT_MODE"},
	{HBG_REG_PORT_ENABLE_ADDR, "PORT_EN"},
	{HBG_REG_PAUSE_ENABLE_ADDR, "PAUSE_EN"},
	{HBG_REG_AN_NEG_STATE_ADDR, "AN_NEG_STATE"},
	{HBG_REG_LINE_LOOP_BACK_ADDR, "LINE_LOOP_BACK"},
	{HBG_REG_CF_CRC_STRIP_ADDR, "CF_CRC_STRIP"},
	{HBG_REG_MODE_CHANGE_EN_ADDR, "MODE_CHANGE_EN"},
	{HBG_REG_LOOP_REG_ADDR, "LOOP_REG"},
	{HBG_REG_RECV_CTRL_ADDR, "RECV_CONTROL"},
	{HBG_REG_VLAN_CODE_ADDR, "VLAN_CODE"},
	{HBG_REG_STATION_ADDR_LOW_0_ADDR, "STATION_ADDR_LOW_0"},
	{HBG_REG_STATION_ADDR_HIGH_0_ADDR, "STATION_ADDR_HIGH_0"},
	{HBG_REG_STATION_ADDR_LOW_1_ADDR, "STATION_ADDR_LOW_1"},
	{HBG_REG_STATION_ADDR_HIGH_1_ADDR, "STATION_ADDR_HIGH_1"},
	{HBG_REG_STATION_ADDR_LOW_2_ADDR, "STATION_ADDR_LOW_2"},
	{HBG_REG_STATION_ADDR_HIGH_2_ADDR, "STATION_ADDR_HIGH_2"},
	{HBG_REG_STATION_ADDR_LOW_3_ADDR, "STATION_ADDR_LOW_3"},
	{HBG_REG_STATION_ADDR_HIGH_3_ADDR, "STATION_ADDR_HIGH_3"},
	{HBG_REG_STATION_ADDR_LOW_4_ADDR, "STATION_ADDR_LOW_4"},
	{HBG_REG_STATION_ADDR_HIGH_4_ADDR, "STATION_ADDR_HIGH_4"},
	{HBG_REG_STATION_ADDR_LOW_5_ADDR, "STATION_ADDR_LOW_5"},
	{HBG_REG_STATION_ADDR_HIGH_5_ADDR, "STATION_ADDR_HIGH_5"},
};

const struct hbg_reg_offset_name_map hbg_pcu_reg_map[] = {
	{HBG_REG_TX_FIFO_THRSLD_ADDR, "CF_TX_FIFO_THRSLD"},
	{HBG_REG_RX_FIFO_THRSLD_ADDR, "CF_RX_FIFO_THRSLD"},
	{HBG_REG_CFG_FIFO_THRSLD_ADDR, "CF_CFG_FIFO_THRSLD"},
	{HBG_REG_CF_INTRPT_MSK_ADDR, "CF_INTRPT_MSK"},
	{HBG_REG_CF_INTRPT_STAT_ADDR, "CF_INTRPT_STAT"},
	{HBG_REG_CF_INTRPT_CLR_ADDR, "CF_INTRPT_CLR"},
	{HBG_REG_TX_BUS_ERR_ADDR_ADDR, "TX_BUS_ERR_ADDR"},
	{HBG_REG_RX_BUS_ERR_ADDR_ADDR, "RX_BUS_ERR_ADDR"},
	{HBG_REG_MAX_FRAME_LEN_ADDR, "MAX_FRAME_LEN"},
	{HBG_REG_DEBUG_ST_MCH_ADDR, "DEBUG_ST_MCH"},
	{HBG_REG_FIFO_CURR_STATUS_ADDR, "FIFO_CURR_STATUS"},
	{HBG_REG_FIFO_HIST_STATUS_ADDR, "FIFO_HIS_STATUS"},
	{HBG_REG_CF_CFF_DATA_NUM_ADDR, "CF_CFF_DATA_NUM"},
	{HBG_REG_CF_TX_PAUSE_ADDR, "CF_TX_PAUSE"},
	{HBG_REG_TX_CFF_ADDR_0_ADDR, "TX_CFF_ADDR_0"},
	{HBG_REG_TX_CFF_ADDR_1_ADDR, "TX_CFF_ADDR_1"},
	{HBG_REG_TX_CFF_ADDR_2_ADDR, "TX_CFF_ADDR_2"},
	{HBG_REG_TX_CFF_ADDR_3_ADDR, "TX_CFF_ADDR_3"},
	{HBG_REG_RX_CFF_ADDR_ADDR, "RX_CFF_ADDR"},
	{HBG_REG_RX_BUF_SIZE_ADDR, "RX_BUF_SIZE"},
	{HBG_REG_BUS_CTRL_ADDR, "BUS_CTRL"},
	{HBG_REG_RX_CTRL_ADDR, "RX_CTRL"},
	{HBG_REG_RX_PKT_MODE_ADDR, "RX_PKT_MODE"},
	{HBG_REG_DBG_ST0_ADDR, "DBG_ST0"},
	{HBG_REG_DBG_ST1_ADDR, "DBG_ST1"},
	{HBG_REG_DBG_ST2_ADDR, "DBG_ST2"},
	{HBG_REG_BUS_RST_EN_ADDR, "BUS_RST_EN"},
	{HBG_REG_CF_IND_TXINT_MSK_ADDR, "CF_IND_TXINT_MSK"},
	{HBG_REG_CF_IND_TXINT_STAT_ADDR, "CF_IND_TXINT_STAT"},
	{HBG_REG_CF_IND_TXINT_CLR_ADDR, "CF_IND_TXINT_CLR"},
	{HBG_REG_CF_IND_RXINT_MSK_ADDR, "CF_IND_RXINT_MSK"},
	{HBG_REG_CF_IND_RXINT_STAT_ADDR, "CF_IND_RXINT_STAT"},
	{HBG_REG_CF_IND_RXINT_CLR_ADDR, "CF_IND_RXINT_CLR"},
};

#define HBG_REG_TYPE_INFO_I(name, base, map) {name, base, map, ARRAY_SIZE(map)}

const struct hbg_reg_type_info hbg_type_infos[] = {
	HBG_REG_TYPE_INFO_I("SPEC", 0, hbg_dev_spec_reg_map),
	HBG_REG_TYPE_INFO_I("MDIO", HBG_REG_MDIO_BASE, hbg_mdio_reg_map),
	HBG_REG_TYPE_INFO_I("GMAC", HBG_REG_SGMII_BASE, hbg_gmac_reg_map),
	HBG_REG_TYPE_INFO_I("PCU", HBG_REG_SGMII_BASE, hbg_pcu_reg_map),
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

static int hbg_ethtool_get_regs_len(struct net_device *netdev)
{
	u32 len = 0;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(hbg_type_infos); i++)
		len += hbg_type_infos[i].reg_num * sizeof(struct hbg_reg_info);

	return len;
}

static u32 hbg_get_reg_info(struct hbg_priv *priv,
			    const struct hbg_reg_type_info *type_info,
			    const struct hbg_reg_offset_name_map *reg_map,
			    struct hbg_reg_info *info)
{
	info->val = hbg_reg_read(priv, reg_map->reg_offset);
	info->offset = reg_map->reg_offset - type_info->offset_base;
	snprintf(info->name, sizeof(info->name),
		 "[%s] %s", type_info->name, reg_map->name);

	return sizeof(*info);
}

static void hbg_ethtool_get_regs(struct net_device *netdev,
				 struct ethtool_regs *regs, void *data)
{
	struct hbg_priv *priv = netdev_priv(netdev);
	const struct hbg_reg_type_info *info;
	u32 i, j, offset = 0;

	regs->version = 0;
	for (i = 0; i < ARRAY_SIZE(hbg_type_infos); i++) {
		info = &hbg_type_infos[i];
		for (j = 0; j < info->reg_num; j++)
			offset += hbg_get_reg_info(priv, info,
						   &info->reg_maps[j],
						   data + offset);
	}
}

static const struct ethtool_ops hbg_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_sset_count		= hbg_ethtool_get_sset_count,
	.get_strings		= hbg_ethtool_get_strings,
	.get_ethtool_stats	= hbg_ethtool_get_stats,
	.get_regs_len		= hbg_ethtool_get_regs_len,
	.get_regs		= hbg_ethtool_get_regs,
};

void hbg_ethtool_set_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &hbg_ethtool_ops;
}
