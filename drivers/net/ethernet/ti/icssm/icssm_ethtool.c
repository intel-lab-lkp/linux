// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSM Ethernet Driver
 *
 * Copyright (C) 2018-2022 Texas Instruments Incorporated - https://www.ti.com/
 *
 */

#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include "icssm_prueth.h"

/* set PRU firmware statistics */
void icssm_emac_set_stats(struct prueth_emac *emac,
			  struct port_statistics *pstats)
{
	void __iomem *dram;

	dram = emac->prueth->mem[emac->dram].va;
	memcpy_toio(dram + STATISTICS_OFFSET, pstats, STAT_SIZE);
}

/* get statistics maintained by the PRU firmware into @pstats */
void icssm_emac_get_stats(struct prueth_emac *emac,
			  struct port_statistics *pstats)
{
	void __iomem *dram;

	dram = emac->prueth->mem[emac->dram].va;
	memcpy_fromio(pstats, dram + STATISTICS_OFFSET, STAT_SIZE);
}

/**
 * icssm_emac_get_drvinfo - Get EMAC driver information
 * @ndev: The network adapter
 * @info: ethtool info structure containing name and version
 *
 * Returns EMAC driver information (name and version)
 */
static void icssm_emac_get_drvinfo(struct net_device *ndev,
				   struct ethtool_drvinfo *info)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;

	strscpy(info->driver, dev_driver_string(prueth->dev),
		sizeof(info->driver));
}

/**
 * icssm_emac_get_link_ksettings - Get EMAC settings
 * @ndev: The network adapter
 * @ecmd: ethtool command
 *
 * Executes ethool get command
 *
 * Return: 0 (Success)
 */
static int icssm_emac_get_link_ksettings(struct net_device *ndev,
					 struct ethtool_link_ksettings *ecmd)
{
	return phy_ethtool_get_link_ksettings(ndev, ecmd);
}

/**
 * icssm_emac_set_link_ksettings - Set EMAC settings
 * @ndev: The EMAC network adapter
 * @ecmd: ethtool command
 *
 * Executes ethool set command
 *
 * Return: 0 (Success)
 */
static int
icssm_emac_set_link_ksettings(struct net_device *ndev,
			      const struct ethtool_link_ksettings *ecmd)
{
	return phy_ethtool_set_link_ksettings(ndev, ecmd);
}

#define PRUETH_STAT_OFFSET(m) offsetof(struct port_statistics, m)

static const struct {
	char string[ETH_GSTRING_LEN];
	u32 offset;
} prueth_ethtool_stats[] = {
	{"txBcast", PRUETH_STAT_OFFSET(tx_bcast)},
	{"txMcast", PRUETH_STAT_OFFSET(tx_mcast)},
	{"txUcast", PRUETH_STAT_OFFSET(tx_ucast)},
	{"txOctets", PRUETH_STAT_OFFSET(tx_octets)},
	{"rxBcast", PRUETH_STAT_OFFSET(rx_bcast)},
	{"rxMcast", PRUETH_STAT_OFFSET(rx_mcast)},
	{"rxUcast", PRUETH_STAT_OFFSET(rx_ucast)},
	{"rxOctets", PRUETH_STAT_OFFSET(rx_octets)},

	{"tx64byte", PRUETH_STAT_OFFSET(tx64byte)},
	{"tx65_127byte", PRUETH_STAT_OFFSET(tx65_127byte)},
	{"tx128_255byte", PRUETH_STAT_OFFSET(tx128_255byte)},
	{"tx256_511byte", PRUETH_STAT_OFFSET(tx256_511byte)},
	{"tx512_1023byte", PRUETH_STAT_OFFSET(tx512_1023byte)},
	{"tx1024byte", PRUETH_STAT_OFFSET(tx1024byte)},
	{"rx64byte", PRUETH_STAT_OFFSET(rx64byte)},
	{"rx65_127byte", PRUETH_STAT_OFFSET(rx65_127byte)},
	{"rx128_255byte", PRUETH_STAT_OFFSET(rx128_255byte)},
	{"rx256_511byte", PRUETH_STAT_OFFSET(rx256_511byte)},
	{"rx512_1023byte", PRUETH_STAT_OFFSET(rx512_1023byte)},
	{"rx1024byte", PRUETH_STAT_OFFSET(rx1024byte)},

	{"lateColl", PRUETH_STAT_OFFSET(late_coll)},
	{"singleColl", PRUETH_STAT_OFFSET(single_coll)},
	{"multiColl", PRUETH_STAT_OFFSET(multi_coll)},
	{"excessColl", PRUETH_STAT_OFFSET(excess_coll)},

	{"rxMisAlignmentFrames", PRUETH_STAT_OFFSET(rx_misalignment_frames)},
	{"stormPrevCounterBC", PRUETH_STAT_OFFSET(stormprev_counter_bc)},
	{"stormPrevCounterMC", PRUETH_STAT_OFFSET(stormprev_counter_mc)},
	{"stormPrevCounterUC", PRUETH_STAT_OFFSET(stormprev_counter_uc)},
	{"macRxError", PRUETH_STAT_OFFSET(mac_rxerror)},
	{"SFDError", PRUETH_STAT_OFFSET(sfd_error)},
	{"defTx", PRUETH_STAT_OFFSET(def_tx)},
	{"macTxError", PRUETH_STAT_OFFSET(mac_txerror)},
	{"rxOverSizedFrames", PRUETH_STAT_OFFSET(rx_oversized_frames)},
	{"rxUnderSizedFrames", PRUETH_STAT_OFFSET(rx_undersized_frames)},
	{"rxCRCFrames", PRUETH_STAT_OFFSET(rx_crc_frames)},
	{"droppedPackets", PRUETH_STAT_OFFSET(dropped_packets)},

	{"txHWQOverFlow", PRUETH_STAT_OFFSET(tx_hwq_overflow)},
	{"txHWQUnderFlow", PRUETH_STAT_OFFSET(tx_hwq_underflow)},
	{"vlanDropped", PRUETH_STAT_OFFSET(vlan_dropped)},
	{"multicastDropped", PRUETH_STAT_OFFSET(multicast_dropped)},
};

static int icssm_emac_get_sset_count(struct net_device *ndev, int stringset)
{
	int a_size;

	switch (stringset) {
	case ETH_SS_STATS:
		a_size = ARRAY_SIZE(prueth_ethtool_stats);

		return a_size;
	default:
		return -EOPNOTSUPP;
	}
}

static void icssm_emac_get_strings(struct net_device *ndev, u32 stringset,
				   u8 *data)
{
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(prueth_ethtool_stats); i++) {
			memcpy(p, prueth_ethtool_stats[i].string,
			       ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	default:
		break;
	}
}

static void icssm_emac_get_ethtool_stats(struct net_device *ndev,
					 struct ethtool_stats *stats, u64 *data)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct port_statistics pstats;
	void *ptr;
	u32 val;
	int i;

	icssm_emac_get_stats(emac, &pstats);

	for (i = 0; i < ARRAY_SIZE(prueth_ethtool_stats); i++) {
		ptr = &pstats;
		ptr += prueth_ethtool_stats[i].offset;
		val = *(u32 *)ptr;
		data[i] = val;
	}
}

static void icssm_emac_get_regs(struct net_device *ndev,
				struct ethtool_regs *regs, void *p)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth *prueth = emac->prueth;

	regs->version = PRUETH_REG_DUMP_GET_VER(prueth);
}

static const struct ethtool_rmon_hist_range icssm_emac_rmon_ranges[] = {
	{    0,   64},
	{   65,  127},
	{  128,  255},
	{  256,  511},
	{  512,  1023},
	{ 1024,  EMAC_MAX_PKTLEN},
	{}
};

static void
icssm_emac_get_rmon_stats(struct net_device *ndev,
			  struct ethtool_rmon_stats *rmon_stats,
			  const struct ethtool_rmon_hist_range **ranges)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct port_statistics pstats;

	*ranges = icssm_emac_rmon_ranges;
	icssm_emac_get_stats(emac, &pstats);

	rmon_stats->undersize_pkts = pstats.rx_undersized_frames;
	rmon_stats->oversize_pkts  = pstats.rx_oversized_frames;

	rmon_stats->hist[0] = pstats.tx64byte;
	rmon_stats->hist[1] = pstats.tx65_127byte;
	rmon_stats->hist[2] = pstats.tx128_255byte;
	rmon_stats->hist[3] = pstats.tx256_511byte;
	rmon_stats->hist[4] = pstats.tx512_1023byte;

	rmon_stats->hist_tx[0] = pstats.rx64byte;
	rmon_stats->hist_tx[1] = pstats.rx65_127byte;
	rmon_stats->hist_tx[2] = pstats.rx128_255byte;
	rmon_stats->hist_tx[3] = pstats.rx256_511byte;
	rmon_stats->hist_tx[4] = pstats.rx1024byte;
}

static void
icssm_emac_get_eth_mac_stats(struct net_device *ndev,
			     struct ethtool_eth_mac_stats *mac_stats)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct port_statistics pstats;

	icssm_emac_get_stats(emac, &pstats);

	mac_stats->LateCollisions = pstats.late_coll;
	mac_stats->SingleCollisionFrames = pstats.single_coll;
	mac_stats->MultipleCollisionFrames = pstats.multi_coll;
}

/* Ethtool support for EMAC adapter */
const struct ethtool_ops emac_ethtool_ops = {
	.get_drvinfo = icssm_emac_get_drvinfo,
	.get_link_ksettings = icssm_emac_get_link_ksettings,
	.set_link_ksettings = icssm_emac_set_link_ksettings,
	.get_link = ethtool_op_get_link,
	.get_sset_count = icssm_emac_get_sset_count,
	.get_strings = icssm_emac_get_strings,
	.get_ethtool_stats = icssm_emac_get_ethtool_stats,
	.get_regs = icssm_emac_get_regs,
	.get_rmon_stats = icssm_emac_get_rmon_stats,
	.get_eth_mac_stats = icssm_emac_get_eth_mac_stats,
};
EXPORT_SYMBOL_GPL(emac_ethtool_ops);
