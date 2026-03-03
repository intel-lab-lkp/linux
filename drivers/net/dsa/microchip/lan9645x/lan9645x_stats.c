// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include <linux/workqueue.h>
#include <linux/debugfs.h>

#include "lan9645x_main.h"
#include "lan9645x_stats.h"

#define LAN9645X_STATS_CHECK_DELAY	(3 * HZ)

static const struct lan9645x_stat_region lan9645x_port_stat_regions[] = {
	/* RX region */
	{ .base_offset = 0x0, .cnt = 67, .cnts_base_idx = 0 },
	/* TX region */
	{ .base_offset = 0x80, .cnt = 48, .cnts_base_idx = 67 },
	/* DR region */
	{ .base_offset = 0x100, .cnt = 18, .cnts_base_idx = 115 },
};

static const struct lan9645x_stat_layout lan9645x_port_stats_layout[] = {
	{ .name = "rx_oct",                .offset = 0x0 },
	{ .name = "rx_uc",                 .offset = 0x1 },
	{ .name = "rx_mc",                 .offset = 0x2 },
	{ .name = "rx_bc",                 .offset = 0x3 },
	{ .name = "rx_short",              .offset = 0x4 },
	{ .name = "rx_frag",               .offset = 0x5 },
	{ .name = "rx_jabber",             .offset = 0x6 },
	{ .name = "rx_crc",                .offset = 0x7 },
	{ .name = "rx_symbol_err",         .offset = 0x8 },
	{ .name = "rx_sz_64",              .offset = 0x9 },
	{ .name = "rx_sz_65_127",          .offset = 0xa },
	{ .name = "rx_sz_128_255",         .offset = 0xb },
	{ .name = "rx_sz_256_511",         .offset = 0xc },
	{ .name = "rx_sz_512_1023",        .offset = 0xd },
	{ .name = "rx_sz_1024_1526",       .offset = 0xe },
	{ .name = "rx_sz_jumbo",           .offset = 0xf },
	{ .name = "rx_pause",              .offset = 0x10 },
	{ .name = "rx_control",            .offset = 0x11 },
	{ .name = "rx_long",               .offset = 0x12 },
	{ .name = "rx_cat_drop",           .offset = 0x13 },
	{ .name = "rx_red_prio_0",         .offset = 0x14 },
	{ .name = "rx_red_prio_1",         .offset = 0x15 },
	{ .name = "rx_red_prio_2",         .offset = 0x16 },
	{ .name = "rx_red_prio_3",         .offset = 0x17 },
	{ .name = "rx_red_prio_4",         .offset = 0x18 },
	{ .name = "rx_red_prio_5",         .offset = 0x19 },
	{ .name = "rx_red_prio_6",         .offset = 0x1a },
	{ .name = "rx_red_prio_7",         .offset = 0x1b },
	{ .name = "rx_yellow_prio_0",      .offset = 0x1c },
	{ .name = "rx_yellow_prio_1",      .offset = 0x1d },
	{ .name = "rx_yellow_prio_2",      .offset = 0x1e },
	{ .name = "rx_yellow_prio_3",      .offset = 0x1f },
	{ .name = "rx_yellow_prio_4",      .offset = 0x20 },
	{ .name = "rx_yellow_prio_5",      .offset = 0x21 },
	{ .name = "rx_yellow_prio_6",      .offset = 0x22 },
	{ .name = "rx_yellow_prio_7",      .offset = 0x23 },
	{ .name = "rx_green_prio_0",       .offset = 0x24 },
	{ .name = "rx_green_prio_1",       .offset = 0x25 },
	{ .name = "rx_green_prio_2",       .offset = 0x26 },
	{ .name = "rx_green_prio_3",       .offset = 0x27 },
	{ .name = "rx_green_prio_4",       .offset = 0x28 },
	{ .name = "rx_green_prio_5",       .offset = 0x29 },
	{ .name = "rx_green_prio_6",       .offset = 0x2a },
	{ .name = "rx_green_prio_7",       .offset = 0x2b },
	{ .name = "rx_assembly_err",       .offset = 0x2c },
	{ .name = "rx_smd_err",            .offset = 0x2d },
	{ .name = "rx_assembly_ok",        .offset = 0x2e },
	{ .name = "rx_merge_frag",         .offset = 0x2f },
	{ .name = "rx_pmac_oct",           .offset = 0x30 },
	{ .name = "rx_pmac_uc",            .offset = 0x31 },
	{ .name = "rx_pmac_mc",            .offset = 0x32 },
	{ .name = "rx_pmac_bc",            .offset = 0x33 },
	{ .name = "rx_pmac_short",         .offset = 0x34 },
	{ .name = "rx_pmac_frag",          .offset = 0x35 },
	{ .name = "rx_pmac_jabber",        .offset = 0x36 },
	{ .name = "rx_pmac_crc",           .offset = 0x37 },
	{ .name = "rx_pmac_symbol_err",    .offset = 0x38 },
	{ .name = "rx_pmac_sz_64",         .offset = 0x39 },
	{ .name = "rx_pmac_sz_65_127",     .offset = 0x3a },
	{ .name = "rx_pmac_sz_128_255",    .offset = 0x3b },
	{ .name = "rx_pmac_sz_256_511",    .offset = 0x3c },
	{ .name = "rx_pmac_sz_512_1023",   .offset = 0x3d },
	{ .name = "rx_pmac_sz_1024_1526",  .offset = 0x3e },
	{ .name = "rx_pmac_sz_jumbo",      .offset = 0x3f },
	{ .name = "rx_pmac_pause",         .offset = 0x40 },
	{ .name = "rx_pmac_control",       .offset = 0x41 },
	{ .name = "rx_pmac_long",          .offset = 0x42 },
	{ .name = "tx_oct",                .offset = 0x80 },
	{ .name = "tx_uc",                 .offset = 0x81 },
	{ .name = "tx_mc",                 .offset = 0x82 },
	{ .name = "tx_bc",                 .offset = 0x83 },
	{ .name = "tx_col",                .offset = 0x84 },
	{ .name = "tx_drop",               .offset = 0x85 },
	{ .name = "tx_pause",              .offset = 0x86 },
	{ .name = "tx_sz_64",              .offset = 0x87 },
	{ .name = "tx_sz_65_127",          .offset = 0x88 },
	{ .name = "tx_sz_128_255",         .offset = 0x89 },
	{ .name = "tx_sz_256_511",         .offset = 0x8a },
	{ .name = "tx_sz_512_1023",        .offset = 0x8b },
	{ .name = "tx_sz_1024_1526",       .offset = 0x8c },
	{ .name = "tx_sz_jumbo",           .offset = 0x8d },
	{ .name = "tx_yellow_prio_0",      .offset = 0x8e },
	{ .name = "tx_yellow_prio_1",      .offset = 0x8f },
	{ .name = "tx_yellow_prio_2",      .offset = 0x90 },
	{ .name = "tx_yellow_prio_3",      .offset = 0x91 },
	{ .name = "tx_yellow_prio_4",      .offset = 0x92 },
	{ .name = "tx_yellow_prio_5",      .offset = 0x93 },
	{ .name = "tx_yellow_prio_6",      .offset = 0x94 },
	{ .name = "tx_yellow_prio_7",      .offset = 0x95 },
	{ .name = "tx_green_prio_0",       .offset = 0x96 },
	{ .name = "tx_green_prio_1",       .offset = 0x97 },
	{ .name = "tx_green_prio_2",       .offset = 0x98 },
	{ .name = "tx_green_prio_3",       .offset = 0x99 },
	{ .name = "tx_green_prio_4",       .offset = 0x9a },
	{ .name = "tx_green_prio_5",       .offset = 0x9b },
	{ .name = "tx_green_prio_6",       .offset = 0x9c },
	{ .name = "tx_green_prio_7",       .offset = 0x9d },
	{ .name = "tx_aged",               .offset = 0x9e },
	{ .name = "tx_llct",               .offset = 0x9f },
	{ .name = "tx_ct",                 .offset = 0xa0 },
	{ .name = "tx_bufdrop",            .offset = 0xa1 },
	{ .name = "tx_mm_hold",            .offset = 0xa2 },
	{ .name = "tx_merge_frag",         .offset = 0xa3 },
	{ .name = "tx_pmac_oct",           .offset = 0xa4 },
	{ .name = "tx_pmac_uc",            .offset = 0xa5 },
	{ .name = "tx_pmac_mc",            .offset = 0xa6 },
	{ .name = "tx_pmac_bc",            .offset = 0xa7 },
	{ .name = "tx_pmac_pause",         .offset = 0xa8 },
	{ .name = "tx_pmac_sz_64",         .offset = 0xa9 },
	{ .name = "tx_pmac_sz_65_127",     .offset = 0xaa },
	{ .name = "tx_pmac_sz_128_255",    .offset = 0xab },
	{ .name = "tx_pmac_sz_256_511",    .offset = 0xac },
	{ .name = "tx_pmac_sz_512_1023",   .offset = 0xad },
	{ .name = "tx_pmac_sz_1024_1526",  .offset = 0xae },
	{ .name = "tx_pmac_sz_jumbo",      .offset = 0xaf },
	{ .name = "dr_local",              .offset = 0x100 },
	{ .name = "dr_tail",               .offset = 0x101 },
	{ .name = "dr_yellow_prio_0",      .offset = 0x102 },
	{ .name = "dr_yellow_prio_1",      .offset = 0x103 },
	{ .name = "dr_yellow_prio_2",      .offset = 0x104 },
	{ .name = "dr_yellow_prio_3",      .offset = 0x105 },
	{ .name = "dr_yellow_prio_4",      .offset = 0x106 },
	{ .name = "dr_yellow_prio_5",      .offset = 0x107 },
	{ .name = "dr_yellow_prio_6",      .offset = 0x108 },
	{ .name = "dr_yellow_prio_7",      .offset = 0x109 },
	{ .name = "dr_green_prio_0",       .offset = 0x10a },
	{ .name = "dr_green_prio_1",       .offset = 0x10b },
	{ .name = "dr_green_prio_2",       .offset = 0x10c },
	{ .name = "dr_green_prio_3",       .offset = 0x10d },
	{ .name = "dr_green_prio_4",       .offset = 0x10e },
	{ .name = "dr_green_prio_5",       .offset = 0x10f },
	{ .name = "dr_green_prio_6",       .offset = 0x110 },
	{ .name = "dr_green_prio_7",       .offset = 0x111 },
};

static const struct lan9645x_view_stats lan9645x_view_stat_cfgs[] = {
	[LAN9645X_STAT_PORTS] = {
	  .name = "ports",
	  .type = LAN9645X_STAT_PORTS,
	  .layout = lan9645x_port_stats_layout,
	  .num_cnts = ARRAY_SIZE(lan9645x_port_stats_layout),
	  .num_indexes = NUM_PHYS_PORTS,
	  .regions = lan9645x_port_stat_regions,
	  .num_regions = ARRAY_SIZE(lan9645x_port_stat_regions),
	},
};

static void __lan9645x_stats_view_idx_update(struct lan9645x *lan9645x,
					     enum lan9645x_view_stat_type vtype,
					     int idx)
{
	struct lan9645x_stat_region region;
	struct lan9645x_view_stats *vstats;
	u64 *idx_counters;
	u32 *region_buf;
	int cntr;
	int err;

	lockdep_assert_held(&lan9645x->stats->hw_lock);

	vstats = lan9645x_get_vstats(lan9645x, vtype);
	if (!vstats || idx < 0 || idx >= vstats->num_indexes)
		return;

	lan_wr(SYS_STAT_CFG_STAT_VIEW_SET(idx), lan9645x, SYS_STAT_CFG);

	idx_counters = STATS_INDEX(vstats, idx);
	region_buf = &vstats->buf[vstats->num_cnts * idx];

	/* Each region for this index contains counters which are at sequential
	 * addresses, so we can use bulk reads to ease lock pressure a bit.
	 */
	for (int r = 0; r < vstats->num_regions; r++) {
		region = vstats->regions[r];
		err = lan_bulk_rd(&region_buf[region.cnts_base_idx], region.cnt,
				  lan9645x, SYS_CNT(region.base_offset));
		if (err) {
			dev_err(lan9645x->dev,
				"stats bulk read err vtype=%d idx=%d err=%d",
				vtype, idx, err);
			return;
		}
	}

	for (cntr = 0; cntr < vstats->num_cnts; cntr++)
		lan9645x_stats_add_cnt(&idx_counters[cntr], region_buf[cntr]);
}

void lan9645x_stats_view_idx_update(struct lan9645x *lan9645x,
				    enum lan9645x_view_stat_type vtype, int idx)
{
	struct lan9645x_stats *s = lan9645x->stats;

	mutex_lock(&s->hw_lock);
	__lan9645x_stats_view_idx_update(lan9645x, vtype, idx);
	mutex_unlock(&s->hw_lock);
}

void lan9645x_stats_view_update(struct lan9645x *lan9645x,
				enum lan9645x_view_stat_type vtype)
{
	struct lan9645x_stats *s = lan9645x->stats;
	struct lan9645x_view_stats *vstats;
	int idx = 0;

	vstats = lan9645x_get_vstats(lan9645x, vtype);
	if (!vstats)
		return;

	switch (vtype) {
	case LAN9645X_STAT_PORTS:
		mutex_lock(&s->hw_lock);
		for (idx = 0; idx < vstats->num_indexes; idx++) {
			if (lan9645x_port_is_used(lan9645x, idx))
				__lan9645x_stats_view_idx_update(lan9645x,
								 vtype, idx);
		}
		mutex_unlock(&s->hw_lock);
		return;
	default:
		return;
	}
}

static void lan9645x_stats_update(struct lan9645x *lan9645x)
{
	for (int vtype = 0; vtype < LAN9645X_STAT_NUM; vtype++)
		lan9645x_stats_view_update(lan9645x, vtype);
}

void lan9645x_stats_get_strings(struct lan9645x *lan9645x, int port,
				u32 stringset, u8 *data)
{
	struct lan9645x_view_stats *port_stats;
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	port_stats = lan9645x_get_vstats(lan9645x, LAN9645X_STAT_PORTS);

	for (i = 0; i < port_stats->num_cnts; i++)
		memcpy(data + i * ETH_GSTRING_LEN, port_stats->layout[i].name,
		       ETH_GSTRING_LEN);
}

int lan9645x_stats_get_sset_count(struct lan9645x *lan9645x, int port, int sset)
{
	struct lan9645x_view_stats *port_stats;

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	port_stats = lan9645x_get_vstats(lan9645x, LAN9645X_STAT_PORTS);

	return port_stats->num_cnts;
}

void lan9645x_stats_get_ethtool_stats(struct lan9645x *lan9645x, int port,
				      u64 *data)
{
	struct lan9645x_view_stats *port_stats;
	int cntr;
	u64 *s;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_stats = lan9645x_get_vstats(lan9645x, LAN9645X_STAT_PORTS);

	s = STATS_INDEX(port_stats, port);

	for (cntr = 0; cntr < port_stats->num_cnts; cntr++)
		*data++ = s[cntr];

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void lan9645x_stats_get_eth_mac_stats(struct lan9645x *lan9645x, int port,
				      struct ethtool_eth_mac_stats *mac_stats)
{
	u64 *port_counters;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_counters = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	mac_stats->FramesTransmittedOK =
		port_counters[SCNT_TX_UC] +
		port_counters[SCNT_TX_MC] +
		port_counters[SCNT_TX_BC] +
		port_counters[SCNT_TX_PMAC_UC] +
		port_counters[SCNT_TX_PMAC_MC] +
		port_counters[SCNT_TX_PMAC_BC];
	mac_stats->SingleCollisionFrames = port_counters[SCNT_TX_COL];
	mac_stats->FramesReceivedOK = port_counters[SCNT_RX_UC] +
				      port_counters[SCNT_RX_MC] +
				      port_counters[SCNT_RX_BC];
	mac_stats->FrameCheckSequenceErrors =
		port_counters[SCNT_RX_CRC] +
		port_counters[SCNT_RX_PMAC_CRC];
	mac_stats->OctetsTransmittedOK =
		port_counters[SCNT_TX_OCT] +
		port_counters[SCNT_TX_PMAC_OCT];
	mac_stats->FramesWithDeferredXmissions = port_counters[SCNT_TX_MM_HOLD];
	mac_stats->OctetsReceivedOK =
		port_counters[SCNT_RX_OCT] +
		port_counters[SCNT_RX_PMAC_OCT];
	mac_stats->MulticastFramesXmittedOK =
		port_counters[SCNT_TX_MC] +
		port_counters[SCNT_TX_PMAC_MC];
	mac_stats->BroadcastFramesXmittedOK =
		port_counters[SCNT_TX_BC] +
		port_counters[SCNT_TX_PMAC_BC];
	mac_stats->MulticastFramesReceivedOK =
		port_counters[SCNT_RX_MC] +
		port_counters[SCNT_RX_PMAC_MC];
	mac_stats->BroadcastFramesReceivedOK =
		port_counters[SCNT_RX_BC] +
		port_counters[SCNT_RX_PMAC_BC];
	mac_stats->InRangeLengthErrors =
		port_counters[SCNT_RX_FRAG] +
		port_counters[SCNT_RX_JABBER] +
		port_counters[SCNT_RX_CRC] +
		port_counters[SCNT_RX_PMAC_FRAG] +
		port_counters[SCNT_RX_PMAC_JABBER] +
		port_counters[SCNT_RX_PMAC_CRC];
	mac_stats->OutOfRangeLengthField = port_counters[SCNT_RX_SHORT] +
					   port_counters[SCNT_RX_PMAC_SHORT] +
					   port_counters[SCNT_RX_LONG] +
					   port_counters[SCNT_RX_PMAC_LONG];
	mac_stats->FrameTooLongErrors =
		port_counters[SCNT_RX_LONG] +
		port_counters[SCNT_RX_PMAC_LONG];

	mutex_unlock(&lan9645x->stats->hw_lock);
}

static const struct ethtool_rmon_hist_range lan9645x_rmon_ranges[] = {
	{    0,     64 },
	{   65,    127 },
	{  128,    255 },
	{  256,    511 },
	{  512,   1023 },
	{ 1024,   1526 },
	{ 1527, 0xffff },
	{}
};

void
lan9645x_stats_get_rmon_stats(struct lan9645x *lan9645x, int port,
			      struct ethtool_rmon_stats *rmon_stats,
			      const struct ethtool_rmon_hist_range **ranges)
{
	u64 *port_cnt;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	rmon_stats->undersize_pkts =
		port_cnt[SCNT_RX_SHORT] +
		port_cnt[SCNT_RX_PMAC_SHORT];
	rmon_stats->oversize_pkts =
		port_cnt[SCNT_RX_LONG] +
		port_cnt[SCNT_RX_PMAC_LONG];
	rmon_stats->fragments =
		port_cnt[SCNT_RX_FRAG] +
		port_cnt[SCNT_RX_PMAC_FRAG];
	rmon_stats->jabbers =
		port_cnt[SCNT_RX_JABBER] +
		port_cnt[SCNT_RX_PMAC_JABBER];

	rmon_stats->hist[0] =
		port_cnt[SCNT_RX_SZ_64] +
		port_cnt[SCNT_RX_PMAC_SZ_64];
	rmon_stats->hist[1] =
		port_cnt[SCNT_RX_SZ_65_127] +
		port_cnt[SCNT_RX_PMAC_SZ_65_127];
	rmon_stats->hist[2] =
		port_cnt[SCNT_RX_SZ_128_255] +
		port_cnt[SCNT_RX_PMAC_SZ_128_255];
	rmon_stats->hist[3] =
		port_cnt[SCNT_RX_SZ_256_511] +
		port_cnt[SCNT_RX_PMAC_SZ_256_511];
	rmon_stats->hist[4] =
		port_cnt[SCNT_RX_SZ_512_1023] +
		port_cnt[SCNT_RX_PMAC_SZ_512_1023];
	rmon_stats->hist[5] =
		port_cnt[SCNT_RX_SZ_1024_1526] +
		port_cnt[SCNT_RX_PMAC_SZ_1024_1526];
	rmon_stats->hist[6] =
		port_cnt[SCNT_RX_SZ_JUMBO] +
		port_cnt[SCNT_RX_PMAC_SZ_JUMBO];

	rmon_stats->hist_tx[0] =
		port_cnt[SCNT_TX_SZ_64] +
		port_cnt[SCNT_TX_PMAC_SZ_64];
	rmon_stats->hist_tx[1] =
		port_cnt[SCNT_TX_SZ_65_127] +
		port_cnt[SCNT_TX_PMAC_SZ_65_127];
	rmon_stats->hist_tx[2] =
		port_cnt[SCNT_TX_SZ_128_255] +
		port_cnt[SCNT_TX_PMAC_SZ_128_255];
	rmon_stats->hist_tx[3] =
		port_cnt[SCNT_TX_SZ_256_511] +
		port_cnt[SCNT_TX_PMAC_SZ_256_511];
	rmon_stats->hist_tx[4] =
		port_cnt[SCNT_TX_SZ_512_1023] +
		port_cnt[SCNT_TX_PMAC_SZ_512_1023];
	rmon_stats->hist_tx[5] =
		port_cnt[SCNT_TX_SZ_1024_1526] +
		port_cnt[SCNT_TX_PMAC_SZ_1024_1526];
	rmon_stats->hist_tx[6] =
		port_cnt[SCNT_TX_SZ_JUMBO] +
		port_cnt[SCNT_TX_PMAC_SZ_JUMBO];

	mutex_unlock(&lan9645x->stats->hw_lock);

	*ranges = lan9645x_rmon_ranges;
}

void lan9645x_stats_get_stats64(struct lan9645x *lan9645x, int port,
				struct rtnl_link_stats64 *stats)
{
	u64 *port_cnt;

	/* Avoid stats update, as this is called very often by DSA. */
	mutex_lock(&lan9645x->stats->hw_lock);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	stats->rx_bytes = port_cnt[SCNT_RX_OCT] +
			  port_cnt[SCNT_RX_PMAC_OCT];

	stats->rx_packets = port_cnt[SCNT_RX_SHORT] +
			    port_cnt[SCNT_RX_FRAG] +
			    port_cnt[SCNT_RX_JABBER] +
			    port_cnt[SCNT_RX_CRC] +
			    port_cnt[SCNT_RX_SYMBOL_ERR] +
			    port_cnt[SCNT_RX_SZ_64] +
			    port_cnt[SCNT_RX_SZ_65_127] +
			    port_cnt[SCNT_RX_SZ_128_255] +
			    port_cnt[SCNT_RX_SZ_256_511] +
			    port_cnt[SCNT_RX_SZ_512_1023] +
			    port_cnt[SCNT_RX_SZ_1024_1526] +
			    port_cnt[SCNT_RX_SZ_JUMBO] +
			    port_cnt[SCNT_RX_LONG] +
			    port_cnt[SCNT_RX_PMAC_SHORT] +
			    port_cnt[SCNT_RX_PMAC_FRAG] +
			    port_cnt[SCNT_RX_PMAC_JABBER] +
			    port_cnt[SCNT_RX_PMAC_SZ_64] +
			    port_cnt[SCNT_RX_PMAC_SZ_65_127] +
			    port_cnt[SCNT_RX_PMAC_SZ_128_255] +
			    port_cnt[SCNT_RX_PMAC_SZ_256_511] +
			    port_cnt[SCNT_RX_PMAC_SZ_512_1023] +
			    port_cnt[SCNT_RX_PMAC_SZ_1024_1526] +
			    port_cnt[SCNT_RX_PMAC_SZ_JUMBO];

	stats->multicast = port_cnt[SCNT_RX_MC] +
			   port_cnt[SCNT_RX_PMAC_MC];

	stats->rx_errors = port_cnt[SCNT_RX_SHORT] +
			   port_cnt[SCNT_RX_FRAG] +
			   port_cnt[SCNT_RX_JABBER] +
			   port_cnt[SCNT_RX_CRC] +
			   port_cnt[SCNT_RX_SYMBOL_ERR] +
			   port_cnt[SCNT_RX_LONG] +
			   port_cnt[SCNT_RX_PMAC_SHORT] +
			   port_cnt[SCNT_RX_PMAC_FRAG] +
			   port_cnt[SCNT_RX_PMAC_JABBER] +
			   port_cnt[SCNT_RX_PMAC_CRC] +
			   port_cnt[SCNT_RX_PMAC_SYMBOL_ERR] +
			   port_cnt[SCNT_RX_PMAC_LONG];

	stats->rx_dropped = port_cnt[SCNT_RX_LONG] +
			    port_cnt[SCNT_DR_LOCAL] +
			    port_cnt[SCNT_DR_TAIL] +
			    port_cnt[SCNT_RX_CAT_DROP] +
			    port_cnt[SCNT_RX_RED_PRIO_0] +
			    port_cnt[SCNT_RX_RED_PRIO_1] +
			    port_cnt[SCNT_RX_RED_PRIO_2] +
			    port_cnt[SCNT_RX_RED_PRIO_3] +
			    port_cnt[SCNT_RX_RED_PRIO_4] +
			    port_cnt[SCNT_RX_RED_PRIO_5] +
			    port_cnt[SCNT_RX_RED_PRIO_6] +
			    port_cnt[SCNT_RX_RED_PRIO_7];

	for (int i = 0; i < LAN9645X_NUM_TC; i++) {
		stats->rx_dropped += port_cnt[SCNT_DR_YELLOW_PRIO_0 + i] +
				     port_cnt[SCNT_DR_GREEN_PRIO_0 + i];
	}

	stats->tx_bytes = port_cnt[SCNT_TX_OCT] +
			  port_cnt[SCNT_TX_PMAC_OCT];

	stats->tx_packets = port_cnt[SCNT_TX_SZ_64] +
			    port_cnt[SCNT_TX_SZ_65_127] +
			    port_cnt[SCNT_TX_SZ_128_255] +
			    port_cnt[SCNT_TX_SZ_256_511] +
			    port_cnt[SCNT_TX_SZ_512_1023] +
			    port_cnt[SCNT_TX_SZ_1024_1526] +
			    port_cnt[SCNT_TX_SZ_JUMBO] +
			    port_cnt[SCNT_TX_PMAC_SZ_64] +
			    port_cnt[SCNT_TX_PMAC_SZ_65_127] +
			    port_cnt[SCNT_TX_PMAC_SZ_128_255] +
			    port_cnt[SCNT_TX_PMAC_SZ_256_511] +
			    port_cnt[SCNT_TX_PMAC_SZ_512_1023] +
			    port_cnt[SCNT_TX_PMAC_SZ_1024_1526] +
			    port_cnt[SCNT_TX_PMAC_SZ_JUMBO];

	stats->tx_dropped = port_cnt[SCNT_TX_DROP] +
			    port_cnt[SCNT_TX_AGED];

	stats->collisions = port_cnt[SCNT_TX_COL];

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void lan9645x_stats_get_eth_phy_stats(struct lan9645x *lan9645x, int port,
				      struct ethtool_eth_phy_stats *phy_stats)
{
	u64 *port_cnt;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	switch (phy_stats->src) {
	case ETHTOOL_MAC_STATS_SRC_EMAC:
		phy_stats->SymbolErrorDuringCarrier =
			port_cnt[SCNT_RX_SYMBOL_ERR];
		break;
	case ETHTOOL_MAC_STATS_SRC_PMAC:
		phy_stats->SymbolErrorDuringCarrier =
			port_cnt[SCNT_RX_PMAC_SYMBOL_ERR];
		break;
	default:
		break;
	}

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void
lan9645x_stats_get_eth_ctrl_stats(struct lan9645x *lan9645x, int port,
				  struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	u64 *port_cnt;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	switch (ctrl_stats->src) {
	case ETHTOOL_MAC_STATS_SRC_EMAC:
		ctrl_stats->MACControlFramesReceived =
			port_cnt[SCNT_RX_CONTROL];
		break;
	case ETHTOOL_MAC_STATS_SRC_PMAC:
		ctrl_stats->MACControlFramesReceived =
			port_cnt[SCNT_RX_PMAC_CONTROL];
		break;
	default:
		break;
	}

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void lan9645x_stats_get_pause_stats(struct lan9645x *lan9645x, int port,
				    struct ethtool_pause_stats *ps)
{
	u64 *port_cnt;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	switch (ps->src) {
	case ETHTOOL_MAC_STATS_SRC_EMAC:
		ps->tx_pause_frames = port_cnt[SCNT_TX_PAUSE];
		ps->rx_pause_frames = port_cnt[SCNT_RX_PAUSE];
		break;
	case ETHTOOL_MAC_STATS_SRC_PMAC:
		ps->tx_pause_frames = port_cnt[SCNT_TX_PMAC_PAUSE];
		ps->rx_pause_frames = port_cnt[SCNT_RX_PMAC_PAUSE];
		break;
	default:
		break;
	}

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void lan9645x_stats_get_mm_stats(struct lan9645x *lan9645x, int port,
				 struct ethtool_mm_stats *stats)
{
	u64 *port_cnt;

	mutex_lock(&lan9645x->stats->hw_lock);

	__lan9645x_stats_view_idx_update(lan9645x, LAN9645X_STAT_PORTS, port);

	port_cnt = STAT_COUNTERS(lan9645x, LAN9645X_STAT_PORTS, port);

	stats->MACMergeFrameAssErrorCount = port_cnt[SCNT_RX_ASSEMBLY_ERR];
	stats->MACMergeFrameSmdErrorCount = port_cnt[SCNT_RX_SMD_ERR];
	stats->MACMergeFrameAssOkCount = port_cnt[SCNT_RX_ASSEMBLY_OK];
	stats->MACMergeFragCountRx = port_cnt[SCNT_RX_MERGE_FRAG];
	stats->MACMergeFragCountTx = port_cnt[SCNT_TX_MERGE_FRAG];
	stats->MACMergeHoldCount = port_cnt[SCNT_TX_MM_HOLD];

	mutex_unlock(&lan9645x->stats->hw_lock);
}

void lan9645x_stats_clear_counters(struct lan9645x *lan9645x,
				   enum lan9645x_view_stat_type type, int idx)
{
	struct lan9645x_view_stats *vstats =
		lan9645x_get_vstats(lan9645x, type);
	u64 *idx_grp;
	int cntr;
	u32 sel;

	switch (type) {
	case LAN9645X_STAT_PORTS:
		/* Drop, TX and RX counters */
		sel = BIT(2) | BIT(1) | BIT(0);
		break;
	case LAN9645X_STAT_ISDX:
		/* ISDX and FRER seq gen */
		sel = BIT(5) | BIT(3);
		break;
	case LAN9645X_STAT_ESDX:
		/* ESDX */
		sel = BIT(6);
		break;
	case LAN9645X_STAT_SFID:
		/* Stream filter */
		sel = BIT(4);
		break;
	default:
		return;
	}

	mutex_lock(&lan9645x->stats->hw_lock);

	lan_wr(SYS_STAT_CFG_STAT_CLEAR_SHOT_SET(sel) |
	       SYS_STAT_CFG_STAT_VIEW_SET(idx),
	       lan9645x, SYS_STAT_CFG);

	idx_grp = STATS_INDEX(vstats, idx);
	for (cntr = 0; cntr < vstats->num_cnts; cntr++)
		idx_grp[cntr] = 0;

	mutex_unlock(&lan9645x->stats->hw_lock);
}

static void lan9645x_check_stats_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct lan9645x_stats *stats;

	stats = container_of(del_work, struct lan9645x_stats, work);

	lan9645x_stats_update(stats->lan9645x);

	queue_delayed_work(stats->queue, &stats->work,
			   LAN9645X_STATS_CHECK_DELAY);
}

static int lan9645x_stats_debugfs_show(struct seq_file *m, void *unused)
{
	struct lan9645x_view_stats *vstats = m->private;
	int idx, cntr;
	size_t total;
	u64 *snap;

	total = vstats->num_cnts * vstats->num_indexes;

	/* Snapshot counters under lock to avoid holding hw_lock during
	 * slow seq_printf output.
	 */
	snap = kmalloc_array(total, sizeof(u64), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;

	mutex_lock(&vstats->stats->hw_lock);
	memcpy(snap, vstats->cnts, total * sizeof(u64));
	mutex_unlock(&vstats->stats->hw_lock);

	for (idx = 0; idx < vstats->num_indexes; idx++) {
		for (cntr = 0; cntr < vstats->num_cnts; cntr++) {
			seq_printf(m, "%s_%d_%-*s %llu\n", vstats->name, idx,
				   30, vstats->layout[cntr].name,
				   snap[vstats->num_cnts * idx + cntr]);
		}
	}

	kfree(snap);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lan9645x_stats_debugfs);

static void lan9645x_stats_debugfs(struct lan9645x *lan9645x,
				   struct dentry *parent)
{
	struct lan9645x_stats *stats = lan9645x->stats;
	struct dentry *dir;
	int i;

	dir = debugfs_create_dir("stats", parent);
	if (PTR_ERR_OR_ZERO(dir))
		return;

	for (i = 0; i < ARRAY_SIZE(stats->view); i++)
		debugfs_create_file(stats->view[i].name, 0444, dir,
				    &stats->view[i],
				    &lan9645x_stats_debugfs_fops);
}

static int lan9645x_view_stat_init(struct lan9645x *lan9645x,
				   struct lan9645x_view_stats *vstat,
				   const struct lan9645x_view_stats *cfg)
{
	size_t total = cfg->num_cnts * cfg->num_indexes;

	memcpy(vstat, cfg, sizeof(*cfg));

	vstat->cnts = devm_kcalloc(lan9645x->dev, total, sizeof(u64),
				   GFP_KERNEL);
	if (!vstat->cnts)
		return -ENOMEM;

	vstat->buf = devm_kcalloc(lan9645x->dev, total, sizeof(u32),
				  GFP_KERNEL);
	if (!vstat->buf)
		return -ENOMEM;

	vstat->stats = lan9645x->stats;

	return 0;
}

int lan9645x_stats_init(struct lan9645x *lan9645x)
{
	const struct lan9645x_view_stats *vs;
	struct lan9645x_stats *stats;
	int err, i;

	lan9645x->stats = devm_kzalloc(lan9645x->dev, sizeof(*stats),
				       GFP_KERNEL);
	if (!lan9645x->stats)
		return -ENOMEM;

	stats = lan9645x->stats;
	stats->lan9645x = lan9645x;

	mutex_init(&stats->hw_lock);
	stats->queue = alloc_ordered_workqueue("lan9645x-stats", 0);
	if (!stats->queue)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(lan9645x_view_stat_cfgs); i++) {
		vs = &lan9645x_view_stat_cfgs[i];

		if (!vs->num_cnts)
			continue;

		err = lan9645x_view_stat_init(lan9645x, &stats->view[vs->type],
					      vs);
		if (err)
			return err;
	}

	INIT_DELAYED_WORK(&stats->work, lan9645x_check_stats_work);
	queue_delayed_work(stats->queue, &stats->work,
			   LAN9645X_STATS_CHECK_DELAY);

	lan9645x_stats_debugfs(lan9645x, lan9645x->debugfs_root);

	return 0;
}

void lan9645x_stats_deinit(struct lan9645x *lan9645x)
{
	cancel_delayed_work_sync(&lan9645x->stats->work);
	destroy_workqueue(lan9645x->stats->queue);
	mutex_destroy(&lan9645x->stats->hw_lock);
	lan9645x->stats->queue = NULL;
}
