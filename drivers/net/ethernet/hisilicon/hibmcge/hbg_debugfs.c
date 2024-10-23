// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2024 Hisilicon Limited.

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/seq_file.h>
#include "hbg_common.h"
#include "hbg_debugfs.h"
#include "hbg_hw.h"
#include "hbg_irq.h"
#include "hbg_txrx.h"

static struct dentry *hbg_dbgfs_root;

struct hbg_dbg_info {
	const char *name;
	int (*read)(struct seq_file *seq, void *data);
};

#define hbg_get_bool_str(state) ((state) ? "true" : "false")

static int hbg_dbg_dev_spec(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);
	struct hbg_dev_specs *specs;

	specs = &priv->dev_specs;
	seq_printf(s, "mac id: %u\n", specs->mac_id);
	seq_printf(s, "phy addr: %u\n", specs->phy_addr);
	seq_printf(s, "mac addr: %pM\n", specs->mac_addr.sa_data);
	seq_printf(s, "vlan layers: %u\n", specs->vlan_layers);
	seq_printf(s, "max frame len: %u\n", specs->max_frame_len);
	seq_printf(s, "min mtu: %u, max mtu: %u\n",
		   specs->min_mtu, specs->max_mtu);
	seq_printf(s, "mdio frequency: %u\n", specs->mdio_frequency);
	seq_printf(s, "uc mac max num: %u\n", specs->uc_mac_num);

	return 0;
}

static void hbg_dbg_ring(struct hbg_priv *priv, struct hbg_ring *ring,
			 struct seq_file *s)
{
	u32 irq_mask = ring->dir == HBG_DIR_TX ? HBG_INT_MSK_TX_B :
						 HBG_INT_MSK_RX_B;

	seq_printf(s, "ring used num: %u\n",
		   hbg_get_queue_used_num(ring));
	seq_printf(s, "ring max num: %u\n", ring->len);
	seq_printf(s, "ring head: %u, tail: %u\n", ring->head, ring->tail);
	seq_printf(s, "fifo used num: %u\n",
		   hbg_hw_get_fifo_used_num(priv, ring->dir));
	seq_printf(s, "fifo max num: %u\n",
		   hbg_get_spec_fifo_max_num(priv, ring->dir));
	seq_printf(s, "irq enabled: %s\n",
		   hbg_get_bool_str(hbg_hw_irq_is_enabled(priv, irq_mask)));
}

static int hbg_dbg_tx_ring(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);

	hbg_dbg_ring(priv, &priv->tx_ring, s);
	return 0;
}

static int hbg_dbg_rx_ring(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);

	hbg_dbg_ring(priv, &priv->rx_ring, s);
	return 0;
}

static int hbg_dbg_irq_info(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);
	struct hbg_irq_info *info;
	u32 i;

	for (i = 0; i < priv->vectors.info_array_len; i++) {
		info = &priv->vectors.info_array[i];
		seq_printf(s,
			   "%-20s: is enabled: %s, print: %s, count: %llu\n",
			   info->name,
			   hbg_get_bool_str(hbg_hw_irq_is_enabled(priv,
								  info->mask)),
			   hbg_get_bool_str(info->need_print),
			   info->count);
	}

	return 0;
}

static const char * const reset_type_str[] = {"None", "FLR", "Function"};

static int hbg_dbg_nic_state(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);

	seq_printf(s, "event handling state: %s\n",
		   hbg_get_bool_str(test_bit(HBG_NIC_STATE_EVENT_HANDLING,
					     &priv->state)));
	seq_printf(s, "need reset state: %s\n",
		   hbg_get_bool_str(test_bit(HBG_NIC_STATE_NEED_RESET,
					     &priv->state)));
	seq_printf(s, "resetting state: %s\n",
		   hbg_get_bool_str(test_bit(HBG_NIC_STATE_RESETTING,
					     &priv->state)));
	seq_printf(s, "reset fail state: %s\n",
		   hbg_get_bool_str(test_bit(HBG_NIC_STATE_RESET_FAIL,
					     &priv->state)));
	seq_printf(s, "last reset type: %s\n",
		   reset_type_str[priv->reset_type]);

	seq_printf(s, "reset fail cnt: %llu\n", priv->stats.reset_fail_cnt);
	seq_printf(s, "tx timeout cnt: %llu\n", priv->stats.tx_timeout_cnt);
	return 0;
}

static int hbg_dbg_mac_table(struct seq_file *s, void *unused)
{
	struct net_device *netdev = dev_get_drvdata(s->private);
	struct hbg_priv *priv = netdev_priv(netdev);
	struct hbg_mac_filter *filter;
	u32 i;

	filter = &priv->filter;
	seq_printf(s, "mac addr max count: %u\n", filter->table_max_len);
	seq_printf(s, "filter enabled: %s\n",
		   hbg_get_bool_str(filter->enabled));
	seq_printf(s, "table overflow: %s\n",
		   hbg_get_bool_str(filter->table_overflow));

	for (i = 0; i < filter->table_max_len; i++) {
		if (is_zero_ether_addr(filter->mac_table[i].addr))
			continue;

		seq_printf(s, "[%u] %pM\n", i, filter->mac_table[i].addr);
	}

	return 0;
}

static const struct hbg_dbg_info hbg_dbg_infos[] = {
	{ "dev_spec", hbg_dbg_dev_spec },
	{ "tx_ring", hbg_dbg_tx_ring },
	{ "rx_ring", hbg_dbg_rx_ring },
	{ "irq_info", hbg_dbg_irq_info },
	{ "nic_state", hbg_dbg_nic_state },
	{ "mac_talbe", hbg_dbg_mac_table },
};

static void hbg_debugfs_uninit(void *data)
{
	debugfs_remove_recursive((struct dentry *)data);
}

int hbg_debugfs_init(struct hbg_priv *priv)
{
	const char *name = pci_name(priv->pdev);
	struct device *dev = &priv->pdev->dev;
	struct dentry *root;
	u32 i;

	root = debugfs_create_dir(name, hbg_dbgfs_root);

	for (i = 0; i < ARRAY_SIZE(hbg_dbg_infos); i++)
		debugfs_create_devm_seqfile(dev, hbg_dbg_infos[i].name,
					    root, hbg_dbg_infos[i].read);

	return devm_add_action_or_reset(dev, hbg_debugfs_uninit, root);
}

void hbg_debugfs_register(void)
{
	hbg_dbgfs_root = debugfs_create_dir("hibmcge", NULL);
}

void hbg_debugfs_unregister(void)
{
	debugfs_remove_recursive(hbg_dbgfs_root);
	hbg_dbgfs_root = NULL;
}
