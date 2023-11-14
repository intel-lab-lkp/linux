// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ethtool operations for a single switch port
 *
 * Copyright (c) 2023, Romain Gantois <romain.gantois@bootlin.com>
 * Based on net/dsa
 */

#include <net/selftests.h>

#include "ipqess_port.h"

static void ipqess_port_get_drvinfo(struct net_device *dev,
				    struct ethtool_drvinfo *drvinfo)
{
	strscpy(drvinfo->driver, "qca8k-ipqess", sizeof(drvinfo->driver));
	strscpy(drvinfo->bus_info, "platform", sizeof(drvinfo->bus_info));
}

static int ipqess_port_nway_reset(struct net_device *dev)
{
	struct ipqess_port *port = netdev_priv(dev);

	return phylink_ethtool_nway_reset(port->pl);
}

static const char ipqess_gstrings_base_stats[][ETH_GSTRING_LEN] = {
	"tx_packets",
	"tx_bytes",
	"rx_packets",
	"rx_bytes",
};

static void ipqess_port_get_strings(struct net_device *dev,
				    u32 stringset, u8 *data)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct qca8k_priv *priv = port->sw->priv;
	int i;

	if (stringset == ETH_SS_STATS) {
		memcpy(data, &ipqess_gstrings_base_stats,
		       sizeof(ipqess_gstrings_base_stats));

		if (stringset != ETH_SS_STATS)
			return;

		for (i = 0; i < priv->info->mib_count; i++)
			memcpy(data + (4 + i) * ETH_GSTRING_LEN,
			       ar8327_mib[i].name,
			       ETH_GSTRING_LEN);

	} else if (stringset == ETH_SS_TEST) {
		net_selftest_get_strings(data);
	}
}

static void ipqess_port_get_ethtool_stats(struct net_device *dev,
					  struct ethtool_stats *stats,
					  uint64_t *data)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct qca8k_priv *priv = port->sw->priv;
	const struct qca8k_mib_desc *mib;
	struct pcpu_sw_netstats *s;
	unsigned int start;
	u32 reg, c, val;
	u32 hi = 0;
	int ret;
	int i;

	for_each_possible_cpu(i) {
		u64 tx_packets, tx_bytes, rx_packets, rx_bytes;

		s = per_cpu_ptr(dev->tstats, i);
		do {
			start = u64_stats_fetch_begin(&s->syncp);
			tx_packets = u64_stats_read(&s->tx_packets);
			tx_bytes = u64_stats_read(&s->tx_bytes);
			rx_packets = u64_stats_read(&s->rx_packets);
			rx_bytes = u64_stats_read(&s->rx_bytes);
		} while (u64_stats_fetch_retry(&s->syncp, start));
		data[0] += tx_packets;
		data[1] += tx_bytes;
		data[2] += rx_packets;
		data[3] += rx_bytes;
	}

	for (c = 0; c < priv->info->mib_count; c++) {
		mib = &ar8327_mib[c];
		reg = QCA8K_PORT_MIB_COUNTER(port->index) + mib->offset;

		ret = qca8k_read(priv, reg, &val);
		if (ret < 0)
			continue;

		if (mib->size == 2) {
			ret = qca8k_read(priv, reg + 4, &hi);
			if (ret < 0)
				continue;
		}

		data[4 + c] = val;
		if (mib->size == 2)
			data[4 + c] |= (u64)hi << 32;
	}
}

static int ipqess_port_get_sset_count(struct net_device *dev, int sset)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct qca8k_priv *priv = port->sw->priv;

	if (sset == ETH_SS_STATS) {
		int count = 0;

		if (sset != ETH_SS_STATS)
			count = 0;
		else
			count = priv->info->mib_count;

		if (count < 0)
			return count;

		return count + 4;
	} else if (sset == ETH_SS_TEST) {
		return net_selftest_get_count();
	}

	return -EOPNOTSUPP;
}

static int ipqess_port_set_wol(struct net_device *dev,
			       struct ethtool_wolinfo *w)
{
	struct ipqess_port *port = netdev_priv(dev);

	return phylink_ethtool_set_wol(port->pl, w);
}

static void ipqess_port_get_wol(struct net_device *dev,
				struct ethtool_wolinfo *w)
{
	struct ipqess_port *port = netdev_priv(dev);

	phylink_ethtool_get_wol(port->pl, w);
}

static int ipqess_port_set_eee(struct net_device *dev, struct ethtool_eee *eee)
{
	struct ipqess_port *port = netdev_priv(dev);
	int ret;
	u32 lpi_en = QCA8K_REG_EEE_CTRL_LPI_EN(port->index);
	struct qca8k_priv *priv = port->sw->priv;
	u32 lpi_ctl1;

	/* Port's PHY and MAC both need to be EEE capable */
	if (!dev->phydev || !port->pl)
		return -ENODEV;

	mutex_lock(&priv->reg_mutex);
	lpi_ctl1 = qca8k_read(priv, QCA8K_REG_EEE_CTRL, &lpi_ctl1);
	if (lpi_ctl1 < 0) {
		mutex_unlock(&priv->reg_mutex);
		return ret;
	}

	if (eee->tx_lpi_enabled && eee->eee_enabled)
		lpi_ctl1 |= lpi_en;
	else
		lpi_ctl1 &= ~lpi_en;
	ret = qca8k_write(priv, QCA8K_REG_EEE_CTRL, lpi_ctl1);
	mutex_unlock(&priv->reg_mutex);

	if (ret)
		return ret;

	return phylink_ethtool_set_eee(port->pl, eee);
}

static int ipqess_port_get_eee(struct net_device *dev, struct ethtool_eee *e)
{
	struct ipqess_port *port = netdev_priv(dev);

	/* Port's PHY and MAC both need to be EEE capable */
	if (!dev->phydev || !port->pl)
		return -ENODEV;

	return phylink_ethtool_get_eee(port->pl, e);
}

static int ipqess_port_get_link_ksettings(struct net_device *dev,
					  struct ethtool_link_ksettings *cmd)
{
	struct ipqess_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_get(port->pl, cmd);
}

static int ipqess_port_set_link_ksettings(struct net_device *dev,
					  const struct ethtool_link_ksettings *cmd)
{
	struct ipqess_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_set(port->pl, cmd);
}

static void ipqess_port_get_pauseparam(struct net_device *dev,
				       struct ethtool_pauseparam *pause)
{
	struct ipqess_port *port = netdev_priv(dev);

	phylink_ethtool_get_pauseparam(port->pl, pause);
}

static int ipqess_port_set_pauseparam(struct net_device *dev,
				      struct ethtool_pauseparam *pause)
{
	struct ipqess_port *port = netdev_priv(dev);

	return phylink_ethtool_set_pauseparam(port->pl, pause);
}

static const struct ethtool_ops ipqess_port_ethtool_ops = {
	.get_drvinfo            = ipqess_port_get_drvinfo,
	.nway_reset             = ipqess_port_nway_reset,
	.get_link               = ethtool_op_get_link,
	.get_strings            = ipqess_port_get_strings,
	.get_ethtool_stats      = ipqess_port_get_ethtool_stats,
	.get_sset_count         = ipqess_port_get_sset_count,
	.self_test              = net_selftest,
	.set_wol                = ipqess_port_set_wol,
	.get_wol                = ipqess_port_get_wol,
	.set_eee                = ipqess_port_set_eee,
	.get_eee                = ipqess_port_get_eee,
	.get_link_ksettings     = ipqess_port_get_link_ksettings,
	.set_link_ksettings     = ipqess_port_set_link_ksettings,
	.get_pauseparam         = ipqess_port_get_pauseparam,
	.set_pauseparam         = ipqess_port_set_pauseparam,
};

void ipqess_port_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ipqess_port_ethtool_ops;
}
