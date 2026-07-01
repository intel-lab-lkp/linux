// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2026 Hisilicon Limited.

#include <linux/netdevice.h>
#include <linux/pci.h>
#include <net/flow_offload.h>

#include "hclge_main.h"
#include "hclge_vf_rep.h"
#include "hclge_fd.h"

static netdev_tx_t hclge_vf_rep_xmit(struct sk_buff *skb,
				     struct net_device *dev)
{
	dev_kfree_skb_any(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static int hclge_vf_rep_get_phys_port_name(struct net_device *dev,
					    char *buf, size_t len)
{
	struct hclge_vf_rep *vf_rep = netdev_priv(dev);
	struct hclge_dev *hdev = vf_rep->hdev;
	int rc;

	rc = snprintf(buf, len, "pf%uvf%u", PCI_FUNC(hdev->pdev->devfn),
		      vf_rep->vport->vport_id - 1);
	if (rc >= len)
		return -EOPNOTSUPP;

	return 0;
}

static int hclge_vf_rep_setup_tc_block_cb(enum tc_setup_type type,
					   void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls_flower = type_data;
	struct hclge_vf_rep *vf_rep = cb_priv;

	if (!tc_cls_can_offload_and_chain0(vf_rep->netdev, type_data))
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		switch (cls_flower->command) {
		case FLOW_CLS_REPLACE:
			return hclge_add_cls_flower_vf(vf_rep, cls_flower);
		case FLOW_CLS_DESTROY:
			return hclge_del_cls_flower_vf(vf_rep, cls_flower);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(hclge_vf_rep_block_cb_list);

static int hclge_vf_rep_setup_tc(struct net_device *dev,
				 enum tc_setup_type type, void *type_data)
{
	struct hclge_vf_rep *vf_rep = netdev_priv(dev);

	switch (type) {
	case TC_SETUP_BLOCK:
		return flow_block_cb_setup_simple(type_data,
						  &hclge_vf_rep_block_cb_list,
						  hclge_vf_rep_setup_tc_block_cb,
						  vf_rep, vf_rep, true);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct net_device_ops hclge_vf_rep_netdev_ops = {
	.ndo_start_xmit		= hclge_vf_rep_xmit,
	.ndo_get_phys_port_name	= hclge_vf_rep_get_phys_port_name,
	.ndo_setup_tc			= hclge_vf_rep_setup_tc,
};

static void hclge_vf_rep_net_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &hclge_vf_rep_netdev_ops;
	ndev->needs_free_netdev = true;
	ndev->features |= NETIF_F_HW_TC;
}

int hclge_create_vf_reps(struct hnae3_ae_dev *ae_dev, int num_vfs)
{
	struct hclge_dev *hdev = ae_dev->priv;
	struct hclge_vf_rep *vf_rep;
	struct net_device *ndev;
	char name[IFNAMSIZ];
	int ret, i;

	if (!num_vfs)
		return 0;

	hdev->vf_reps = kcalloc(num_vfs, sizeof(struct hclge_vf_rep *),
				GFP_KERNEL);
	if (!hdev->vf_reps)
		return -ENOMEM;

	for (i = 0; i < num_vfs; i++) {
		snprintf(name, IFNAMSIZ, "%s_rep%d",
			 hdev->vport[0].nic.netdev->name, i);
		ndev = alloc_netdev(sizeof(struct hclge_vf_rep), name,
				    NET_NAME_UNKNOWN, ether_setup);
		if (!ndev) {
			ret = -ENOMEM;
			goto err;
		}

		hclge_vf_rep_net_setup(ndev);

		vf_rep = netdev_priv(ndev);
		vf_rep->hdev = hdev;
		vf_rep->vport = &hdev->vport[i + HCLGE_VF_VPORT_START_NUM];
		vf_rep->netdev = ndev;

		ret = register_netdev(ndev);
		if (ret) {
			free_netdev(ndev);
			goto err;
		}

		hdev->vf_reps[i] = vf_rep;
	}

	hdev->num_vf_reps = num_vfs;
	return 0;

err:
	while (i--)
		unregister_netdev(hdev->vf_reps[i]->netdev);
	kfree(hdev->vf_reps);
	hdev->vf_reps = NULL;
	return ret;
}

void hclge_destroy_vf_reps(struct hnae3_ae_dev *ae_dev)
{
	struct hclge_dev *hdev = ae_dev->priv;
	int i;

	if (!hdev->vf_reps)
		return;

	for (i = 0; i < hdev->num_vf_reps; i++) {
		if (hdev->vf_reps[i])
			unregister_netdev(hdev->vf_reps[i]->netdev);
	}

	kfree(hdev->vf_reps);
	hdev->vf_reps = NULL;
	hdev->num_vf_reps = 0;
}
