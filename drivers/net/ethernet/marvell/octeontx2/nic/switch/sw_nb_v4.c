// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/switchdev.h>
#include <net/netevent.h>
#include <net/arp.h>
#include <net/route.h>
#include <linux/inetdevice.h>
#include <net/nexthop.h>

#include "../otx2_reg.h"
#include "../otx2_common.h"
#include "../otx2_struct.h"
#include "../cn10k.h"
#include "sw_nb.h"
#include "sw_fdb.h"
#include "sw_fib.h"
#include "sw_fl.h"
#include "sw_nb_v4.h"

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)

int sw_nb_v4_netdev_event(struct notifier_block *unused,
			  unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct netdev_hw_addr *dev_addr;
	struct net_device *pf_dev;
	struct in_device *idev;
	struct in_ifaddr *ifa;
	struct fib_entry *entry;
	struct otx2_nic *pf;

	idev = __in_dev_get_rtnl(dev);
	if (!idev || !idev->ifa_list)
		return NOTIFY_DONE;

	if (!sw_nb_is_valid_dev(dev))
		return NOTIFY_DONE;

	/* Switch offload supports a single IPv4 address per interface for
	 * now. Only the head of ifa_list is offloaded on netdev events;
	 * secondary addresses are not supported by the hardware path.
	 */
	ifa = rtnl_dereference(idev->ifa_list);

	entry = kcalloc(1, sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return NOTIFY_DONE;

	entry->cmd = sw_nb_inetaddr_event_to_otx2_event(event, dev);
	entry->dst = ifa->ifa_address;
	entry->dst_len = 32;
	entry->mac_valid = 1;
	entry->host = 1;

	pf_dev = sw_nb_resolve_pf_dev(dev);
	if (!pf_dev) {
		kfree(entry);
		return NOTIFY_DONE;
	}

	if (netif_is_bridge_master(dev)) {
		entry->bridge = 1;
	} else if (is_vlan_dev(dev)) {
		entry->vlan_valid = 1;
		entry->vlan_tag = cpu_to_be16(vlan_dev_vlan_id(dev));
	}

	/* Switch offload is only enabled on OcteonTX2/CN10K SoCs. pf_dev is an
	 * octeontx2 PF or representor netdev, so netdev_priv() is otx2_nic even
	 * though sw_nb_is_cavium_dev() matches the shared Cavium PCI vendor ID.
	 */
	pf = netdev_priv(pf_dev);
	entry->port_id = pf->pcifunc;

	for_each_dev_addr(dev, dev_addr) {
		ether_addr_copy(entry->mac, dev_addr->addr);
		break;
	}

	netdev_dbg(dev, "%s: pushing netdev event from HOST interface address %pI4, %pM, dev=%s\n",
		   __func__, &entry->dst, entry->mac, dev->name);
	sw_fib_add_to_list(pf_dev, entry, 1);

	return NOTIFY_DONE;
}

int sw_nb_v4_inetaddr_event(struct notifier_block *nb,
			    unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = (struct in_ifaddr *)ptr;
	struct net_device *dev = ifa->ifa_dev->dev;
	struct netdev_hw_addr *dev_addr;
	struct net_device *pf_dev;
	struct fib_entry *entry;
	struct otx2_nic *pf;

	if (event != NETDEV_CHANGE &&
	    event != NETDEV_UP &&
	    event != NETDEV_DOWN) {
		return NOTIFY_DONE;
	}

	if (!sw_nb_is_valid_dev(dev))
		return NOTIFY_DONE;

	/* Use ifa from the notifier; idev->ifa_list is already empty when the
	 * final address is unlinked before NETDEV_DOWN is delivered.
	 */
	entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NOTIFY_DONE;

	entry->cmd = sw_nb_inetaddr_event_to_otx2_event(event, dev);
	entry->dst = ifa->ifa_address;
	entry->dst_len = 32;
	entry->mac_valid = 1;
	entry->host = 1;

	pf_dev = sw_nb_resolve_pf_dev(dev);
	if (!pf_dev) {
		kfree(entry);
		return NOTIFY_DONE;
	}

	if (netif_is_bridge_master(dev)) {
		entry->bridge = 1;
	} else if (is_vlan_dev(dev)) {
		entry->vlan_valid = 1;
		entry->vlan_tag = cpu_to_be16(vlan_dev_vlan_id(dev));
	}

	pf = netdev_priv(pf_dev);
	entry->port_id = pf->pcifunc;

	for_each_dev_addr(dev, dev_addr) {
		ether_addr_copy(entry->mac, dev_addr->addr);
		break;
	}

	netdev_dbg(dev, "%s: pushing inetaddr event from HOST interface address %pI4, %pM, %s\n",
		   __func__, &entry->dst, entry->mac, dev->name);

	sw_fib_add_to_list(pf_dev, entry, 1);
	return NOTIFY_DONE;
}

int sw_nb_v4_fib_event(struct notifier_block *nb,
		       unsigned long event, void *ptr)
{
	struct fib_entry_notifier_info *fen_info = ptr;
	struct net_device *host_pf_dev = NULL;
	struct netdev_hw_addr *dev_addr;
	struct net_device *nh_pf_dev;
	struct fib_nh_common *nhc;
	struct neighbour *neigh;
	struct fib_entry *entry;
	struct net_device *dev;
	struct fib_nh *fib_nh;
	struct fib_info *fi;
	struct otx2_nic *pf;
	int i, cnt, nhs;
	__be32 *haddr;
	int hcnt = 0;

	/* Process only UNICAST routes add or del */
	if (fen_info->type != RTN_UNICAST)
		return NOTIFY_DONE;

	fi = fen_info->fi;
	if (!fi)
		return NOTIFY_DONE;

	nhs = fib_info_num_path(fi);

	if (fi->fib_nh_is_v6) {
		if (nhs > 0) {
			nhc = fib_info_nhc(fi, 0);

			if (nhc->nhc_dev)
				netdev_dbg(nhc->nhc_dev,
					   "%s: Received v6 notification\n",
					   __func__);
		}
		return NOTIFY_DONE;
	}

	if (!nhs)
		return NOTIFY_DONE;

	haddr = kcalloc(nhs, sizeof(*haddr), GFP_ATOMIC);
	if (!haddr)
		return NOTIFY_DONE;

	for (i = 0; i < nhs; i++) {
		nhc = fib_info_nhc(fi, i);
		fib_nh = container_of(nhc, struct fib_nh, nh_common);
		dev = fib_nh->fib_nh_dev;

		if (!dev)
			continue;

		if (dev->type != ARPHRD_ETHER)
			continue;

		if (!sw_nb_is_valid_dev(dev))
			continue;

		nh_pf_dev = sw_nb_resolve_pf_dev(dev);
		if (!nh_pf_dev)
			continue;

		entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
		if (!entry)
			break;

		entry->cmd = sw_nb_fib_event_to_otx2_event(event, dev);
		entry->dst = htonl(fen_info->dst);
		entry->dst_len = fen_info->dst_len;
		entry->gw = fib_nh->fib_nh_gw4;

		if (netif_is_bridge_master(dev)) {
			entry->bridge = 1;
		} else if (is_vlan_dev(dev)) {
			entry->vlan_valid = 1;
			entry->vlan_tag = cpu_to_be16(vlan_dev_vlan_id(dev));
		}

		pf = netdev_priv(nh_pf_dev);
		entry->port_id = pf->pcifunc;

		/* Point-to-point routes, including default routes with no
		 * gateway, are not supported for switch offload.
		 */
		if (!fib_nh->fib_nh_gw4) {
			if (!entry->dst && !entry->dst_len) {
				kfree(entry);
				continue;
			}
			sw_fib_add_to_list(nh_pf_dev, entry, 1);
			continue;
		}

		entry->gw_valid = 1;

		if (fib_nh->nh_saddr)
			haddr[hcnt++] = fib_nh->nh_saddr;

		rcu_read_lock();
		neigh = ip_neigh_gw4(fib_nh->fib_nh_dev, fib_nh->fib_nh_gw4);
		if (IS_ERR_OR_NULL(neigh)) {
			rcu_read_unlock();
			kfree(entry);
			continue;
		}

		if (is_valid_ether_addr(neigh->ha)) {
			entry->mac_valid = 1;
			neigh_ha_snapshot(entry->mac, neigh, fib_nh->fib_nh_dev);
		}
		rcu_read_unlock();

		netdev_dbg(dev, "%s: FIB route Rule cmd=%llu dst=%pI4 dst_len=%u gw=%pI4\n",
			   __func__, entry->cmd, &entry->dst, entry->dst_len,
			   &entry->gw);
		sw_fib_add_to_list(nh_pf_dev, entry, 1);
	}

	if (!hcnt) {
		kfree(haddr);
		return NOTIFY_DONE;
	}

	for (i = 0; i < hcnt; i++) {
		host_pf_dev = NULL;
		for (cnt = 0; cnt < nhs; cnt++) {
			nhc = fib_info_nhc(fi, cnt);
			fib_nh = container_of(nhc, struct fib_nh, nh_common);
			if (fib_nh->nh_saddr == haddr[i]) {
				host_pf_dev = sw_nb_resolve_pf_dev(fib_nh->fib_nh_dev);
				break;
			}
		}

		if (!host_pf_dev)
			continue;

		entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
		if (!entry)
			break;

		pf = netdev_priv(host_pf_dev);
		entry->cmd = sw_nb_fib_event_to_otx2_event(event, host_pf_dev);
		entry->dst = haddr[i];
		entry->dst_len = 32;
		entry->mac_valid = 1;
		entry->host = 1;
		entry->port_id = pf->pcifunc;

		rcu_read_lock();
		for_each_dev_addr(host_pf_dev, dev_addr) {
			ether_addr_copy(entry->mac, dev_addr->addr);
			break;
		}
		rcu_read_unlock();

		netdev_dbg(host_pf_dev,
			   "%s: FIB host Rule cmd=%llu dst=%pI4 dst_len=%u gw=%pI4 %s\n",
			   __func__, entry->cmd, &entry->dst, entry->dst_len,
			   &entry->gw, host_pf_dev->name);
		sw_fib_add_to_list(host_pf_dev, entry, 1);
	}

	kfree(haddr);
	return NOTIFY_DONE;
}

int sw_nb_net_v4_neigh_update(struct notifier_block *nb,
			      unsigned long event, void *ptr)
{
	struct net_device *pf_dev;
	struct neighbour *n = ptr;
	struct fib_entry *entry;
	struct otx2_nic *pf;

	if (n->tbl != &arp_tbl)
		return NOTIFY_DONE;

	if (!sw_nb_is_valid_dev(n->dev))
		return NOTIFY_DONE;

	entry = kcalloc(1, sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NOTIFY_DONE;

	entry->cmd = OTX2_NEIGH_UPDATE;
	entry->dst = *(__be32 *)n->primary_key;
	entry->dst_len = n->tbl->key_len * 8;
	entry->mac_valid = 1;
	entry->nud_state = n->nud_state;
	neigh_ha_snapshot(entry->mac, n, n->dev);

	pf_dev = sw_nb_resolve_pf_dev(n->dev);
	if (!pf_dev) {
		kfree(entry);
		return NOTIFY_DONE;
	}

	if (netif_is_bridge_master(n->dev)) {
		entry->bridge = 1;
	} else if (is_vlan_dev(n->dev)) {
		entry->vlan_valid = 1;
		entry->vlan_tag = cpu_to_be16(vlan_dev_vlan_id(n->dev));
	}

	pf = netdev_priv(pf_dev);
	entry->port_id = pf->pcifunc;

	sw_fib_add_to_list(pf_dev, entry, 1);
	return NOTIFY_DONE;
}

#endif
