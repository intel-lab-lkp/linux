// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 Facebook

#include <linux/debugfs.h>
#include <linux/random.h>
#include <net/netdev_queues.h>

#include "netdevsim.h"

static void
nsim_get_pause_stats(struct net_device *dev,
		     struct ethtool_pause_stats *pause_stats)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (ns->ethtool.pauseparam.report_stats_rx)
		pause_stats->rx_pause_frames = 1;
	if (ns->ethtool.pauseparam.report_stats_tx)
		pause_stats->tx_pause_frames = 2;
}

static void
nsim_get_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct netdevsim *ns = netdev_priv(dev);

	pause->autoneg = 0; /* We don't support ksettings, so can't pretend */
	pause->rx_pause = ns->ethtool.pauseparam.rx;
	pause->tx_pause = ns->ethtool.pauseparam.tx;
}

static int
nsim_set_pauseparam(struct net_device *dev, struct ethtool_pauseparam *pause)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (pause->autoneg)
		return -EINVAL;

	ns->ethtool.pauseparam.rx = pause->rx_pause;
	ns->ethtool.pauseparam.tx = pause->tx_pause;
	return 0;
}

static int nsim_get_coalesce(struct net_device *dev,
			     struct ethtool_coalesce *coal,
			     struct kernel_ethtool_coalesce *kernel_coal,
			     struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(coal, &ns->ethtool.coalesce, sizeof(ns->ethtool.coalesce));
	return 0;
}

static int nsim_set_coalesce(struct net_device *dev,
			     struct ethtool_coalesce *coal,
			     struct kernel_ethtool_coalesce *kernel_coal,
			     struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(&ns->ethtool.coalesce, coal, sizeof(ns->ethtool.coalesce));
	return 0;
}

static void nsim_get_ringparam(struct net_device *dev,
			       struct ethtool_ringparam *ring,
			       struct kernel_ethtool_ringparam *kernel_ring,
			       struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	memcpy(ring, &ns->ethtool.ring, sizeof(ns->ethtool.ring));
	kernel_ring->hds_thresh_max = NSIM_HDS_THRESHOLD_MAX;

	if (dev->cfg->hds_config == ETHTOOL_TCP_DATA_SPLIT_UNKNOWN)
		kernel_ring->tcp_data_split = ETHTOOL_TCP_DATA_SPLIT_ENABLED;
}

static int nsim_set_ringparam(struct net_device *dev,
			      struct ethtool_ringparam *ring,
			      struct kernel_ethtool_ringparam *kernel_ring,
			      struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	ns->ethtool.ring.rx_pending = ring->rx_pending;
	ns->ethtool.ring.rx_jumbo_pending = ring->rx_jumbo_pending;
	ns->ethtool.ring.rx_mini_pending = ring->rx_mini_pending;
	ns->ethtool.ring.tx_pending = ring->tx_pending;
	return 0;
}

static void
nsim_get_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct netdevsim *ns = netdev_priv(dev);

	ch->max_combined = ns->nsim_bus_dev->num_queues;
	ch->combined_count = ns->ethtool.channels;
}

static void
nsim_wake_queues(struct net_device *dev)
{
	struct netdevsim *ns = netdev_priv(dev);
	struct netdevsim *peer;

	synchronize_net();
	netif_tx_wake_all_queues(dev);

	rcu_read_lock();
	peer = rcu_dereference(ns->peer);
	if (peer)
		netif_tx_wake_all_queues(peer->netdev);
	rcu_read_unlock();
}

static int
nsim_set_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct netdevsim *ns = netdev_priv(dev);
	int err;

	err = netif_set_real_num_queues(dev, ch->combined_count,
					ch->combined_count);
	if (err)
		return err;

	ns->ethtool.channels = ch->combined_count;

	/* Only wake up queues if devices are linked */
	if (rcu_access_pointer(ns->peer))
		nsim_wake_queues(dev);

	return 0;
}

static int
nsim_get_fecparam(struct net_device *dev, struct ethtool_fecparam *fecparam)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (ns->ethtool.get_err)
		return -ns->ethtool.get_err;
	memcpy(fecparam, &ns->ethtool.fec, sizeof(ns->ethtool.fec));
	return 0;
}

static int
nsim_set_fecparam(struct net_device *dev, struct ethtool_fecparam *fecparam)
{
	struct netdevsim *ns = netdev_priv(dev);
	u32 fec;

	if (ns->ethtool.set_err)
		return -ns->ethtool.set_err;
	memcpy(&ns->ethtool.fec, fecparam, sizeof(ns->ethtool.fec));
	fec = fecparam->fec;
	if (fec == ETHTOOL_FEC_AUTO)
		fec |= ETHTOOL_FEC_OFF;
	fec |= ETHTOOL_FEC_NONE;
	ns->ethtool.fec.active_fec = 1 << (fls(fec) - 1);
	return 0;
}

static const struct ethtool_fec_hist_range netdevsim_fec_ranges[] = {
	{ 0, 0},
	{ 1, 3},
	{ 4, 7},
	{ 0, 0}
};

static void
nsim_get_fec_stats(struct net_device *dev, struct ethtool_fec_stats *fec_stats,
		   struct ethtool_fec_hist *hist)
{
	struct ethtool_fec_hist_value *values = hist->values;

	hist->ranges = netdevsim_fec_ranges;

	fec_stats->corrected_blocks.total = 123;
	fec_stats->uncorrectable_blocks.total = 4;

	values[0].per_lane[0] = 125;
	values[0].per_lane[1] = 120;
	values[0].per_lane[2] = 100;
	values[0].per_lane[3] = 100;
	values[1].sum = 12;
	values[2].sum = 2;
	values[2].per_lane[0] = 2;
	values[2].per_lane[1] = 0;
	values[2].per_lane[2] = 0;
	values[2].per_lane[3] = 0;
}

static void nsim_fill_mac_lb_entry(struct netdevsim *ns,
				   struct ethtool_loopback_entry *entry)
{
	memset(entry, 0, sizeof(*entry));
	entry->component = ETHTOOL_LOOPBACK_COMPONENT_MAC;
	strscpy(entry->name, "mac", sizeof(entry->name));
	entry->supported = ns->ethtool.mac_lb.supported;
	entry->direction = ns->ethtool.mac_lb.direction;
}

static int nsim_get_loopback(struct net_device *dev, const char *name,
			     u32 id, struct ethtool_loopback_entry *entry)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (strcmp(name, "mac"))
		return -EOPNOTSUPP;

	nsim_fill_mac_lb_entry(ns, entry);
	return 0;
}

static int nsim_get_loopback_by_index(struct net_device *dev, u32 index,
				      struct ethtool_loopback_entry *entry)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (index > 0)
		return -EOPNOTSUPP;

	nsim_fill_mac_lb_entry(ns, entry);
	return 0;
}

static int nsim_set_loopback(struct net_device *dev,
			     const struct ethtool_loopback_entry *entry,
			     struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);

	if (strcmp(entry->name, "mac")) {
		NL_SET_ERR_MSG(extack, "Unknown MAC loopback name");
		return -EOPNOTSUPP;
	}

	if (ns->ethtool.mac_lb.direction == entry->direction)
		return 0;

	ns->ethtool.mac_lb.direction = entry->direction;
	return 1;
}

static u8 *nsim_module_eeprom_ptr(struct netdevsim *ns,
				  const struct ethtool_module_eeprom *page_data,
				  u32 *len)
{
	u32 offset;
	u8 page;

	if (page_data->offset < NSIM_MODULE_EEPROM_PAGE_LEN) {
		page = 0;
		offset = page_data->offset;
	} else {
		page = page_data->page;
		offset = page_data->offset - NSIM_MODULE_EEPROM_PAGE_LEN;
	}

	*len = min_t(u32, page_data->length,
		     NSIM_MODULE_EEPROM_PAGE_LEN - offset);
	return ns->ethtool.module.pages[page] + offset;
}

static int
nsim_get_module_eeprom_by_page(struct net_device *dev,
			       const struct ethtool_module_eeprom *page_data,
			       struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);
	u32 len;
	u8 *ptr;

	if (ns->ethtool.module.get_err)
		return -ns->ethtool.module.get_err;

	ptr = nsim_module_eeprom_ptr(ns, page_data, &len);
	if (!ptr)
		return -EINVAL;

	memcpy(page_data->data, ptr, len);

	return len;
}

static int
nsim_set_module_eeprom_by_page(struct net_device *dev,
			       const struct ethtool_module_eeprom *page_data,
			       struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = netdev_priv(dev);
	u32 len;
	u8 *ptr;

	if (ns->ethtool.module.set_err)
		return -ns->ethtool.module.set_err;

	ptr = nsim_module_eeprom_ptr(ns, page_data, &len);
	if (!ptr)
		return -EINVAL;

	memcpy(ptr, page_data->data, len);

	return 0;
}

static int nsim_get_ts_info(struct net_device *dev,
			    struct kernel_ethtool_ts_info *info)
{
	struct netdevsim *ns = netdev_priv(dev);

	info->phc_index = mock_phc_index(ns->phc);

	return 0;
}

static const struct ethtool_ops nsim_ethtool_ops = {
	.supported_coalesce_params	= ETHTOOL_COALESCE_ALL_PARAMS,
	.supported_ring_params		= ETHTOOL_RING_USE_TCP_DATA_SPLIT |
					  ETHTOOL_RING_USE_HDS_THRS,
	.get_pause_stats	        = nsim_get_pause_stats,
	.get_pauseparam		        = nsim_get_pauseparam,
	.set_pauseparam		        = nsim_set_pauseparam,
	.set_coalesce			= nsim_set_coalesce,
	.get_coalesce			= nsim_get_coalesce,
	.get_ringparam			= nsim_get_ringparam,
	.set_ringparam			= nsim_set_ringparam,
	.get_channels			= nsim_get_channels,
	.set_channels			= nsim_set_channels,
	.get_fecparam			= nsim_get_fecparam,
	.set_fecparam			= nsim_set_fecparam,
	.get_fec_stats			= nsim_get_fec_stats,
	.get_ts_info			= nsim_get_ts_info,
	.get_module_eeprom_by_page	= nsim_get_module_eeprom_by_page,
	.set_module_eeprom_by_page	= nsim_set_module_eeprom_by_page,
	.get_loopback			= nsim_get_loopback,
	.get_loopback_by_index		= nsim_get_loopback_by_index,
	.set_loopback			= nsim_set_loopback,
};

static void nsim_ethtool_ring_init(struct netdevsim *ns)
{
	ns->ethtool.ring.rx_pending = 512;
	ns->ethtool.ring.rx_max_pending = 4096;
	ns->ethtool.ring.rx_jumbo_max_pending = 4096;
	ns->ethtool.ring.rx_mini_max_pending = 4096;
	ns->ethtool.ring.tx_pending = 512;
	ns->ethtool.ring.tx_max_pending = 4096;
}

void nsim_ethtool_init(struct netdevsim *ns)
{
	struct dentry *ethtool, *dir;
	int i;

	ns->netdev->ethtool_ops = &nsim_ethtool_ops;

	nsim_ethtool_ring_init(ns);

	ns->ethtool.pauseparam.report_stats_rx = true;
	ns->ethtool.pauseparam.report_stats_tx = true;

	ns->ethtool.fec.fec = ETHTOOL_FEC_NONE;
	ns->ethtool.fec.active_fec = ETHTOOL_FEC_NONE;

	ns->ethtool.channels = ns->nsim_bus_dev->num_queues;

	ethtool = debugfs_create_dir("ethtool", ns->nsim_dev_port->ddir);

	debugfs_create_u32("get_err", 0600, ethtool, &ns->ethtool.get_err);
	debugfs_create_u32("set_err", 0600, ethtool, &ns->ethtool.set_err);

	dir = debugfs_create_dir("pause", ethtool);
	debugfs_create_bool("report_stats_rx", 0600, dir,
			    &ns->ethtool.pauseparam.report_stats_rx);
	debugfs_create_bool("report_stats_tx", 0600, dir,
			    &ns->ethtool.pauseparam.report_stats_tx);

	dir = debugfs_create_dir("ring", ethtool);
	debugfs_create_u32("rx_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_max_pending);
	debugfs_create_u32("rx_jumbo_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_jumbo_max_pending);
	debugfs_create_u32("rx_mini_max_pending", 0600, dir,
			   &ns->ethtool.ring.rx_mini_max_pending);
	debugfs_create_u32("tx_max_pending", 0600, dir,
			   &ns->ethtool.ring.tx_max_pending);

	dir = debugfs_create_dir("module", ethtool);
	debugfs_create_u32("get_err", 0600, dir, &ns->ethtool.module.get_err);
	debugfs_create_u32("set_err", 0600, dir, &ns->ethtool.module.set_err);

	dir = debugfs_create_dir("pages", dir);
	for (i = 0; i < NSIM_MODULE_EEPROM_PAGES; i++) {
		char name[8];

		ns->ethtool.module.page_blobs[i].data =
			ns->ethtool.module.pages[i];
		ns->ethtool.module.page_blobs[i].size =
			NSIM_MODULE_EEPROM_PAGE_LEN;

		snprintf(name, sizeof(name), "%u", i);
		debugfs_create_blob(name, 0600, dir,
				    &ns->ethtool.module.page_blobs[i]);
	}

	ns->ethtool.mac_lb.supported = ETHTOOL_LOOPBACK_DIRECTION_LOCAL |
				       ETHTOOL_LOOPBACK_DIRECTION_REMOTE;

	dir = debugfs_create_dir("mac_lb", ethtool);
	debugfs_create_u32("supported", 0600, dir,
			   &ns->ethtool.mac_lb.supported);
	debugfs_create_u32("direction", 0600, dir,
			   &ns->ethtool.mac_lb.direction);
}
