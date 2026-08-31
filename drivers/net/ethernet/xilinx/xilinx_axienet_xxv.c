// SPDX-License-Identifier: GPL-2.0-only
/*
 * XXV (10G/25G) support
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include <linux/netdevice.h>
#include <linux/phylink.h>
#include <linux/string.h>

#include "xilinx_axienet.h"
#include "xilinx_axienet_xxv.h"

/* readl_poll_timeout() sleep interval (us) and 1 ms total timeout for GT
 * reset-done and PCS block-lock polls. Values from ZynqMP/Versal bring-up;
 * PG210 does not specify these bounds.
 */
#define XXV_LINK_POLL_INTERVAL_US	10

/* Number of XXV (10G/25G) MAC registers exposed via ethtool -d.
 * Covers configuration and status registers per Xilinx PG210.
 */
static const u32 axienet_xxv_reg_dump_offsets[XXV_REGS_N] = {
	XXV_GT_RESET_OFFSET,
	XXV_RESET_OFFSET,
	XXV_MODE_OFFSET,
	XXV_TC_OFFSET,
	XXV_RCW1_OFFSET,
	XXV_JUM_OFFSET,
	XXV_VL_LENGTH_OFFSET,
	XXV_TICKREG_OFFSET,
	XXV_CONFIG_REVISION,
	XXV_CONFIG_1588_OFFSET,
	XXV_CONFIG_TX_FLOW_CTRL1_OFFSET,
	XXV_CONFIG_RX_FLOW_CTRL1_OFFSET,
	XXV_CONFIG_RX_FLOW_CTRL2_OFFSET,
	XXV_CONFIG_RSFEC_OFFSET,
	XXV_CONFIG_FEC_OFFSET,
	XXV_AN_CTL1_OFFSET,
	XXV_AN_CTL2_OFFSET,
	XXV_AN_ABILITY_OFFSET,
	XXV_LT_CTL1_OFFSET,
	XXV_SWITCH_CORE_SPEED_OFFSET,
	XXV_CONFIG_1588_32BIT_OFFSET,
	XXV_TX_CONFIG_1588_OFFSET,
	XXV_RX_CONFIG_1588_OFFSET,
	XXV_GTWIZ_CTRL_OFFSET,
	XXV_STATRX_STATUS0_OFFSET,
	XXV_RX_STATUS_REG1,
	XXV_STATRX_STATUS2_OFFSET,
	XXV_STATRX_BLKLCK_OFFSET,
	XXV_STAT_RX_RSFEC_STATUS_OFFSET,
	XXV_STAT_RX_FEC_STATUS_OFFSET,
	XXV_STAT_TX_RSFEC_STATUS_OFFSET,
	XXV_STAT_TX_FLOW_CTRL1_OFFSET,
	XXV_STAT_RX_FLOW_CTRL1_OFFSET,
	XXV_STAT_AN_STS_OFFSET,
	XXV_STAT_AN_LP_STATUS_OFFSET,
	XXV_STAT_AN_LINK_CTL_OFFSET,
	XXV_STAT_LT_STATUS1_OFFSET,
	XXV_STAT_LT_STATUS2_OFFSET,
	XXV_STAT_LT_STATUS3_OFFSET,
	XXV_STAT_LT_STATUS4_OFFSET,
	XXV_STAT_LT_COEFF0_OFFSET,
	XXV_STAT_RX_VALID_CTRL_CODE_OFFSET,
	XXV_STAT_CORE_SPEED_OFFSET,
	XXV_STAT_TSN_OFFSET,
	XXV_STAT_GTWIZ_OFFSET,
	XXV_STAT_AN_LINK_CTL2_OFFSET,
};

/* Option table for setting up XXV Ethernet hardware options */
static struct axienet_option xxvenet_options[] = {
	{ /* Turn on FCS stripping on receive packets */
		.opt = XAE_OPTION_FCS_STRIP,
		.reg = XXV_RCW1_OFFSET,
		.m_or = XXV_RCW1_FCS_MASK,
	}, { /* Turn on FCS insertion on transmit packets */
		.opt = XAE_OPTION_FCS_INSERT,
		.reg = XXV_TC_OFFSET,
		.m_or = XXV_TC_FCS_MASK,
	}, { /* Enable transmitter */
		.opt = XAE_OPTION_TXEN,
		.reg = XXV_TC_OFFSET,
		.m_or = XXV_TC_TX_MASK,
	}, { /* Enable receiver */
		.opt = XAE_OPTION_RXEN,
		.reg = XXV_RCW1_OFFSET,
		.m_or = XXV_RCW1_RX_MASK,
	},
	{}
};

/* PG210 statistics-block offsets indexed by enum xxv_stat. Each entry is
 * the 32-bit LSB register address; the corresponding MSB is at +4.
 */
static const u32 axienet_xxv_stat_offsets[XXV_STAT_COUNT] = {
	[XXV_STAT_RX_TOTAL_PACKETS]	 = XXV_STAT_RX_TOTAL_PACKETS_OFFSET,
	[XXV_STAT_RX_TOTAL_GOOD_PACKETS] = XXV_STAT_RX_TOTAL_GOOD_PACKETS_OFFSET,
	[XXV_STAT_RX_TOTAL_BYTES]	 = XXV_STAT_RX_TOTAL_BYTES_OFFSET,
	[XXV_STAT_RX_TOTAL_GOOD_BYTES]	 = XXV_STAT_RX_TOTAL_GOOD_BYTES_OFFSET,
	[XXV_STAT_RX_BAD_FCS]		 = XXV_STAT_RX_BAD_FCS_OFFSET,
	[XXV_STAT_RX_INRANGEERR]	 = XXV_STAT_RX_INRANGEERR_OFFSET,
	[XXV_STAT_RX_MULTICAST]		 = XXV_STAT_RX_MULTICAST_OFFSET,
	[XXV_STAT_RX_BROADCAST]		 = XXV_STAT_RX_BROADCAST_OFFSET,
	[XXV_STAT_RX_UNDERSIZE]		 = XXV_STAT_RX_UNDERSIZE_OFFSET,
	[XXV_STAT_RX_FRAGMENT]		 = XXV_STAT_RX_FRAGMENT_OFFSET,
	[XXV_STAT_RX_JABBER]		 = XXV_STAT_RX_JABBER_OFFSET,
	[XXV_STAT_RX_OVERSIZE]		 = XXV_STAT_RX_OVERSIZE_OFFSET,
	[XXV_STAT_RX_PAUSE]		 = XXV_STAT_RX_PAUSE_OFFSET,
	[XXV_STAT_TX_TOTAL_PACKETS]	 = XXV_STAT_TX_TOTAL_PACKETS_OFFSET,
	[XXV_STAT_TX_TOTAL_GOOD_PACKETS] = XXV_STAT_TX_TOTAL_GOOD_PACKETS_OFFSET,
	[XXV_STAT_TX_TOTAL_BYTES]	 = XXV_STAT_TX_TOTAL_BYTES_OFFSET,
	[XXV_STAT_TX_TOTAL_GOOD_BYTES]	 = XXV_STAT_TX_TOTAL_GOOD_BYTES_OFFSET,
	[XXV_STAT_TX_BAD_FCS]		 = XXV_STAT_TX_BAD_FCS_OFFSET,
	[XXV_STAT_TX_FRAME_ERROR]	 = XXV_STAT_TX_FRAME_ERROR_OFFSET,
	[XXV_STAT_TX_MULTICAST]		 = XXV_STAT_TX_MULTICAST_OFFSET,
	[XXV_STAT_TX_BROADCAST]		 = XXV_STAT_TX_BROADCAST_OFFSET,
	[XXV_STAT_TX_PAUSE]		 = XXV_STAT_TX_PAUSE_OFFSET,
};

/* XXV (PG210) counters exposed via ethtool -S. Only counters that have no
 * standard uAPI are listed here; the totals include errored frames, so they
 * are distinct from the good-frame counts in rtnl_link_stats64 and the
 * IEEE MAC statistics. Every other XXV counter is reported through the
 * dedicated stats64, pause, MAC and RMON callbacks instead.
 */
static const enum xxv_stat axienet_xxv_priv_stats[] = {
	XXV_STAT_RX_TOTAL_PACKETS,
	XXV_STAT_RX_TOTAL_BYTES,
	XXV_STAT_TX_TOTAL_PACKETS,
	XXV_STAT_TX_TOTAL_BYTES,
	XXV_STAT_TX_BAD_FCS,
	XXV_STAT_TX_FRAME_ERROR,
};

static const char axienet_xxv_ethtool_stats_strings[][ETH_GSTRING_LEN] = {
	"RX Total Packets",
	"RX Total Bytes",
	"TX Total Packets",
	"TX Total Bytes",
	"TX Bad FCS",
	"TX Frame Error",
};

static u64 axienet_xxv_read_counter(struct axienet_local *lp, enum xxv_stat stat)
{
	u32 off = axienet_xxv_stat_offsets[stat];
	u32 lsb, msb;

	lsb = axienet_ior(lp, off);
	msb = axienet_ior(lp, off + XXV_STAT_MSB_OFFSET);

	return ((u64)msb << 32) | lsb;
}

static bool axienet_xxv_ip_has_gtwiz_status(u32 ip_version)
{
	u8 minor = FIELD_GET(XXV_MIN_MASK, ip_version);
	u8 maj = FIELD_GET(XXV_MAJ_MASK, ip_version);

	if (maj == XXV_IP_VER_GTWIZ_MAJ_MIN)
		return minor >= XXV_IP_VER_GTWIZ_MIN_MIN;
	return maj > XXV_IP_VER_GTWIZ_MAJ_MIN;
}

/**
 * axienet_xxv_setoptions - Set XXV MAC options from the option table
 * @ndev: Pointer to the net_device structure
 * @options: Option flags to apply
 */
static void axienet_xxv_setoptions(struct net_device *ndev, u32 options)
{
	struct axienet_local *lp = netdev_priv(ndev);
	struct axienet_option *tp = xxvenet_options;
	u32 reg;

	while (tp->opt) {
		reg = axienet_ior(lp, tp->reg) & ~tp->m_or;
		if (options & tp->opt)
			reg |= tp->m_or;
		axienet_iow(lp, tp->reg, reg);
		tp++;
	}

	lp->options |= options;
}

/**
 * axienet_xxv_probe_init - Initialize XXV-specific state at probe time
 * @lp: Pointer to the axienet_local structure
 */
static void axienet_xxv_probe_init(struct axienet_local *lp)
{
	lp->xxv_ip_version = axienet_ior(lp, XXV_CONFIG_REVISION);
	lp->features |= XAE_FEATURE_STATS;
}

/**
 * axienet_xxv_gt_reset - Pulse the XXV GT reset line
 * @lp: Pointer to the axienet_local structure
 */
static void axienet_xxv_gt_reset(struct axienet_local *lp)
{
	u32 val;

	/* Reset GT */
	val = axienet_ior(lp, XXV_GT_RESET_OFFSET);
	val |= XXV_GT_RESET_MASK;
	axienet_iow(lp, XXV_GT_RESET_OFFSET, val);
	/* Allow 1 ms for the GT reset to settle (see timeout note above) */
	usleep_range(1000, 2000);
	val = axienet_ior(lp, XXV_GT_RESET_OFFSET);
	val &= ~XXV_GT_RESET_MASK;
	axienet_iow(lp, XXV_GT_RESET_OFFSET, val);
}

/**
 * axienet_xxv_poll_link_ready - Wait for XXV GT and PCS block lock
 * @ndev: Pointer to the net_device structure
 *
 * Poll GT wizard reset-done on IP v3.2+, then RX PCS block lock. Block-lock
 * failure is logged but not propagated: it depends on a live link partner and
 * reference clock, so an unplugged cable must not fail device bring-up.
 *
 * Return: 0 on success, or a negative error if GT reset-done times out.
 */
static int axienet_xxv_poll_link_ready(struct net_device *ndev)
{
	struct axienet_local *lp = netdev_priv(ndev);
	u32 val;
	int ret;

	/* Confirm XXV Ethernet is up: on IP v3.2+, wait for GT
	 * reset-done before further register access, then poll until
	 * RX PCS block lock is asserted.
	 */
	if (axienet_xxv_ip_has_gtwiz_status(lp->xxv_ip_version)) {
		ret = readl_poll_timeout(lp->regs + XXV_STAT_GTWIZ_OFFSET,
					 val,
					 (val & XXV_GTWIZ_RESET_DONE) == XXV_GTWIZ_RESET_DONE,
					 XXV_LINK_POLL_INTERVAL_US,
					 DELAY_OF_ONE_MILLISEC);
		if (ret) {
			netdev_err(ndev, "XXV MAC GT reset not complete! Cross-check the MAC ref clock configuration\n");
			return ret;
		}
	}

	ret = readl_poll_timeout(lp->regs + XXV_STATRX_BLKLCK_OFFSET,
				 val, (val & XXV_RX_BLKLCK_MASK),
				 XXV_LINK_POLL_INTERVAL_US,
				 DELAY_OF_ONE_MILLISEC);
	if (ret)
		netdev_err(ndev, "XXV MAC block lock not complete! Cross-check the MAC ref clock configuration\n");

	return 0;
}

/**
 * axienet_xxv_mac_init - XXV MAC-specific bring-up after the DMA reset
 * @ndev: Pointer to the net_device structure
 *
 * Return: 0 on success or a negative error number otherwise.
 */
static int axienet_xxv_mac_init(struct net_device *ndev)
{
	struct axienet_local *lp = netdev_priv(ndev);
	int ret;

	ret = axienet_xxv_poll_link_ready(ndev);
	if (ret)
		return ret;

	lp->options |= XAE_OPTION_FCS_STRIP;
	lp->options |= XAE_OPTION_FCS_INSERT;

	return 0;
}

static void axienet_xxv_phylink_set_capabilities(struct axienet_local *lp,
						 struct phylink_config *cfg)
{
	u32 core_speed;
	bool rtsw;

	core_speed = axienet_ior(lp, XXV_STAT_CORE_SPEED_OFFSET);
	/* Bit[1:0]: 00=25G, 01=10G, 10=runtime-switchable 25G,
	 * 11=runtime-switchable 10G. A runtime-switchable core can operate at
	 * either speed, so advertise both; a fixed core advertises only its
	 * configured speed.
	 */
	rtsw = core_speed & XXV_STAT_CORE_SPEED_RTSW_MASK;

	if (rtsw || (core_speed & XXV_STAT_CORE_SPEED_10G_MASK)) {
		cfg->mac_capabilities |= MAC_10000FD;
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  cfg->supported_interfaces);
	}

	if (rtsw || !(core_speed & XXV_STAT_CORE_SPEED_10G_MASK)) {
		cfg->mac_capabilities |= MAC_25000FD;
		__set_bit(PHY_INTERFACE_MODE_25GBASER,
			  cfg->supported_interfaces);
	}
}

static void axienet_xxv_pcs_get_state(struct phylink_pcs *pcs,
				      unsigned int neg_mode,
				      struct phylink_link_state *state)
{
	struct axienet_local *lp = pcs_to_axienet_local(pcs);

	state->duplex = DUPLEX_FULL;
	state->an_complete = !!(axienet_ior(lp, XXV_STAT_AN_STS_OFFSET) &
				XXV_AN_COMPLETE_MASK);
	state->link = axienet_ior(lp, XXV_RX_STATUS_REG1) &
		      XXV_RX_STATUS_MASK;

	if (axienet_ior(lp, XXV_STAT_CORE_SPEED_OFFSET) &
	    XXV_STAT_CORE_SPEED_10G_MASK)
		state->speed = SPEED_10000;
	else
		state->speed = SPEED_25000;
}

static int axienet_xxv_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
				  phy_interface_t interface,
				  const unsigned long *advertising,
				  bool permit_pause_to_mac)
{
	return 0;
}

static const struct phylink_pcs_ops axienet_xxv_pcs_ops = {
	.pcs_get_state = axienet_xxv_pcs_get_state,
	.pcs_config = axienet_xxv_pcs_config,
};

/**
 * axienet_xxv_get_regs - Dump XXV MAC registers for ethtool
 * @lp: Pointer to the axienet_local structure
 * @data: Buffer for register values (zeroed and sized by the caller)
 */
static void axienet_xxv_get_regs(struct axienet_local *lp, u32 *data)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(axienet_xxv_reg_dump_offsets); i++)
		data[i] = axienet_ior(lp, axienet_xxv_reg_dump_offsets[i]);
}

static int axienet_10g25g_clk_init(struct axienet_local *lp)
{
	struct device *dev = lp->dev;
	int ret;

	/* The register, RX and GT DRP clocks are mandatory for MAC+PCS
	 * operation, so fetch them as required.
	 */
	lp->axi_clk = devm_clk_get_enabled(dev, "s_axi");
	if (IS_ERR(lp->axi_clk))
		return dev_err_probe(dev, PTR_ERR(lp->axi_clk),
				     "could not get AXI clock\n");

	lp->misc_clks[0].id = "rx_core";
	lp->misc_clks[1].id = "drp";

	ret = devm_clk_bulk_get_enable(dev, 2, lp->misc_clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not get/enable XXV clocks\n");

	return 0;
}

/**
 * axienet_xxv_stats_update - Latch and accumulate XXV hardware counters
 * @lp: Pointer to the axienet_local structure
 *
 * The XXV statistics counters are clear-on-tick (PG210): writing TICK_REG
 * latches the internal accumulators into the readable STAT_*_LSB/MSB registers
 * and clears the internal accumulators. Each post-TICK read therefore returns
 * the count for the interval since the previous TICK, so software accumulates
 * those intervals into lp->xxv_stat_base. A last-counter delta (as used by the
 * free-running 1G MAC path) is not needed.
 */
static void axienet_xxv_stats_update(struct axienet_local *lp)
{
	u64 counter[XXV_STAT_COUNT];
	enum xxv_stat stat;

	/* Latch the clear-on-tick snapshot and read the counters outside the
	 * seqcount write section, so only the accumulator updates run inside it
	 * and the reader retry window stays short.
	 */
	axienet_iow(lp, XXV_TICKREG_OFFSET, XXV_TICKREG_STATEN_MASK);
	for (stat = 0; stat < XXV_STAT_COUNT; stat++)
		counter[stat] = axienet_xxv_read_counter(lp, stat);

	write_seqcount_begin(&lp->hw_stats_seqcount);
	for (stat = 0; stat < XXV_STAT_COUNT; stat++)
		lp->xxv_stat_base[stat] += counter[stat];
	write_seqcount_end(&lp->hw_stats_seqcount);
}

/**
 * axienet_xxv_get_stats64 - Fill rtnl_link_stats64 from XXV counters
 * @lp: Pointer to the axienet_local structure
 * @stats: Output rtnl_link_stats64 structure to populate
 */
static void axienet_xxv_get_stats64(struct axienet_local *lp,
				    struct rtnl_link_stats64 *stats)
{
	unsigned int start;
	u64 tx_hw_errors;

	do {
		start = read_seqcount_begin(&lp->hw_stats_seqcount);
		stats->rx_crc_errors = lp->xxv_stat_base[XXV_STAT_RX_BAD_FCS];
		/* Both in-range length errors and frame-too-long (oversize)
		 * frames are IEEE 802.3 length errors, so fold them together
		 * into rx_length_errors; rx_over_errors is reserved for receiver
		 * FIFO/ring overflow, which the XXV MAC does not expose.
		 */
		stats->rx_length_errors =
			lp->xxv_stat_base[XXV_STAT_RX_INRANGEERR] +
			lp->xxv_stat_base[XXV_STAT_RX_OVERSIZE];
		stats->rx_errors = lp->xxv_stat_base[XXV_STAT_RX_UNDERSIZE] +
				   lp->xxv_stat_base[XXV_STAT_RX_FRAGMENT] +
				   lp->xxv_stat_base[XXV_STAT_RX_JABBER] +
				   stats->rx_crc_errors +
				   stats->rx_length_errors;
		stats->multicast = lp->xxv_stat_base[XXV_STAT_RX_MULTICAST];
		tx_hw_errors = lp->xxv_stat_base[XXV_STAT_TX_BAD_FCS] +
			       lp->xxv_stat_base[XXV_STAT_TX_FRAME_ERROR];
	} while (read_seqcount_retry(&lp->hw_stats_seqcount, start));

	/* Fold the HW TX error count into the software tx_errors seeded from
	 * dev->stats by the caller. The XXV xmit path bumps dev->stats.tx_errors
	 * for frames dropped on pad failure before they reach the MAC, while the
	 * HW counters only cover frames the MAC actually transmitted; the two are
	 * disjoint. This runs once, outside the seqcount retry loop, so a racing
	 * stats update cannot make the accumulation re-add on retry.
	 */
	stats->tx_errors += tx_hw_errors;
}

/**
 * axienet_xxv_get_ethtool_stats - Copy XXV counters for ethtool -S
 * @lp: Pointer to the axienet_local structure
 * @data: Output buffer sized for ARRAY_SIZE(axienet_xxv_priv_stats) u64 values
 */
static void axienet_xxv_get_ethtool_stats(struct axienet_local *lp, u64 *data)
{
	unsigned int i, start;

	do {
		start = read_seqcount_begin(&lp->hw_stats_seqcount);
		for (i = 0; i < ARRAY_SIZE(axienet_xxv_priv_stats); i++)
			data[i] = lp->xxv_stat_base[axienet_xxv_priv_stats[i]];
	} while (read_seqcount_retry(&lp->hw_stats_seqcount, start));
}

/**
 * axienet_xxv_get_strings - Copy XXV ethtool statistics name strings
 * @data: Output buffer for the statistics name strings
 */
static void axienet_xxv_get_strings(u8 *data)
{
	memcpy(data, axienet_xxv_ethtool_stats_strings,
	       sizeof(axienet_xxv_ethtool_stats_strings));
}

/**
 * axienet_xxv_get_sset_count - Number of XXV ethtool statistics
 *
 * Return: Count of XXV hardware statistics reported via ethtool -S.
 */
static int axienet_xxv_get_sset_count(void)
{
	return ARRAY_SIZE(axienet_xxv_ethtool_stats_strings);
}

/**
 * axienet_xxv_get_pause_stats - Fill ethtool pause frame statistics
 * @lp: Pointer to the axienet_local structure
 * @pause_stats: Output ethtool_pause_stats structure to populate
 */
static void axienet_xxv_get_pause_stats(struct axienet_local *lp,
					struct ethtool_pause_stats *pause_stats)
{
	unsigned int start;

	do {
		start = read_seqcount_begin(&lp->hw_stats_seqcount);
		pause_stats->tx_pause_frames =
			lp->xxv_stat_base[XXV_STAT_TX_PAUSE];
		pause_stats->rx_pause_frames =
			lp->xxv_stat_base[XXV_STAT_RX_PAUSE];
	} while (read_seqcount_retry(&lp->hw_stats_seqcount, start));
}

/**
 * axienet_xxv_get_eth_mac_stats - Fill IEEE 802.3 MAC statistics
 * @lp: Pointer to the axienet_local structure
 * @mac_stats: Output ethtool_eth_mac_stats structure to populate
 */
static void axienet_xxv_get_eth_mac_stats(struct axienet_local *lp,
					  struct ethtool_eth_mac_stats *mac_stats)
{
	unsigned int start;

	do {
		start = read_seqcount_begin(&lp->hw_stats_seqcount);
		mac_stats->FramesTransmittedOK =
			lp->xxv_stat_base[XXV_STAT_TX_TOTAL_GOOD_PACKETS];
		mac_stats->FramesReceivedOK =
			lp->xxv_stat_base[XXV_STAT_RX_TOTAL_GOOD_PACKETS];
		mac_stats->OctetsTransmittedOK =
			lp->xxv_stat_base[XXV_STAT_TX_TOTAL_GOOD_BYTES];
		mac_stats->OctetsReceivedOK =
			lp->xxv_stat_base[XXV_STAT_RX_TOTAL_GOOD_BYTES];
		mac_stats->FrameCheckSequenceErrors =
			lp->xxv_stat_base[XXV_STAT_RX_BAD_FCS];
		mac_stats->MulticastFramesXmittedOK =
			lp->xxv_stat_base[XXV_STAT_TX_MULTICAST];
		mac_stats->BroadcastFramesXmittedOK =
			lp->xxv_stat_base[XXV_STAT_TX_BROADCAST];
		mac_stats->MulticastFramesReceivedOK =
			lp->xxv_stat_base[XXV_STAT_RX_MULTICAST];
		mac_stats->BroadcastFramesReceivedOK =
			lp->xxv_stat_base[XXV_STAT_RX_BROADCAST];
		mac_stats->InRangeLengthErrors =
			lp->xxv_stat_base[XXV_STAT_RX_INRANGEERR];
	} while (read_seqcount_retry(&lp->hw_stats_seqcount, start));
}

/**
 * axienet_xxv_get_rmon_stats - Fill RMON statistics and histogram ranges
 * @lp: Pointer to the axienet_local structure
 * @rmon_stats: Output ethtool_rmon_stats structure to populate
 * @ranges: Set to the RMON histogram range table
 */
static void axienet_xxv_get_rmon_stats(struct axienet_local *lp,
				       struct ethtool_rmon_stats *rmon_stats,
				       const struct ethtool_rmon_hist_range **ranges)
{
	unsigned int start;

	do {
		start = read_seqcount_begin(&lp->hw_stats_seqcount);
		rmon_stats->undersize_pkts =
			lp->xxv_stat_base[XXV_STAT_RX_UNDERSIZE];
		rmon_stats->oversize_pkts =
			lp->xxv_stat_base[XXV_STAT_RX_OVERSIZE];
		rmon_stats->fragments =
			lp->xxv_stat_base[XXV_STAT_RX_FRAGMENT];
		rmon_stats->jabbers =
			lp->xxv_stat_base[XXV_STAT_RX_JABBER];
	} while (read_seqcount_retry(&lp->hw_stats_seqcount, start));

	/* XXV currently exposes only aggregate RMON counters, not per-bin
	 * histogram buckets. Keep ranges NULL until histogram bins are wired.
	 */
	*ranges = NULL;
}

const struct axienet_config axienet_10g25g_config = {
	.sw_padding = true,
	.internal_pcs = true,
	.regs_n = XXV_REGS_N,
	.clk_init = axienet_10g25g_clk_init,
	.setoptions = axienet_xxv_setoptions,
	.probe_init = axienet_xxv_probe_init,
	.gt_reset = axienet_xxv_gt_reset,
	.mac_init = axienet_xxv_mac_init,
	.get_regs = axienet_xxv_get_regs,
	.phylink_set_caps = axienet_xxv_phylink_set_capabilities,
	.pcs_ops = &axienet_xxv_pcs_ops,
	.stats_update = axienet_xxv_stats_update,
	.get_stats64 = axienet_xxv_get_stats64,
	.get_ethtool_stats = axienet_xxv_get_ethtool_stats,
	.get_strings = axienet_xxv_get_strings,
	.get_sset_count = axienet_xxv_get_sset_count,
	.get_pause_stats = axienet_xxv_get_pause_stats,
	.get_eth_mac_stats = axienet_xxv_get_eth_mac_stats,
	.get_rmon_stats = axienet_xxv_get_rmon_stats,
};
