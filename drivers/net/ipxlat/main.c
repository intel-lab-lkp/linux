// SPDX-License-Identifier: GPL-2.0
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2024- Alberto Leiva Popper <ydahhrk@gmail.com>
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Alberto Leiva Popper <ydahhrk@gmail.com>
 *		Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#include <linux/module.h>

#include <net/ip.h>

#include "ipxlpriv.h"
#include "main.h"

MODULE_AUTHOR("Alberto Leiva Popper <ydahhrk@gmail.com>");
MODULE_AUTHOR("Antonio Quartulli <antonio@mandelbit.com>");
MODULE_AUTHOR("Daniel Gröber <dxld@darkboxed.org>");
MODULE_AUTHOR("Ralf Lici <ralf@mandelbit.com>");
MODULE_DESCRIPTION("IPv6<>IPv4 translation virtual netdev support (SIIT)");
MODULE_LICENSE("GPL");

static int ipxlat_dev_init(struct net_device *dev)
{
	struct ipxlat_priv *ipxlat = netdev_priv(dev);
	int err;

	ipxlat->dev = dev;
	/* default xlat-prefix6 is 64:ff9b::/96 */
	ipxlat->xlat_prefix6.addr.s6_addr32[0] = htonl(0x0064ff9b);
	ipxlat->xlat_prefix6.addr.s6_addr32[1] = 0;
	ipxlat->xlat_prefix6.addr.s6_addr32[2] = 0;
	ipxlat->xlat_prefix6.addr.s6_addr32[3] = 0;
	ipxlat->xlat_prefix6.len = 96;
	ipxlat->lowest_ipv6_mtu = 1280;
	mutex_init(&ipxlat->cfg_lock);

	err = gro_cells_init(&ipxlat->gro_cells, dev);
	if (unlikely(err))
		return err;

	return 0;
}

static void ipxlat_dev_uninit(struct net_device *dev)
{
	struct ipxlat_priv *ipxlat = netdev_priv(dev);

	gro_cells_destroy(&ipxlat->gro_cells);
}

static int ipxlat_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	dev_dstats_tx_dropped(dev);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops ipxlat_netdev_ops = {
	.ndo_init = ipxlat_dev_init,
	.ndo_uninit = ipxlat_dev_uninit,
	.ndo_start_xmit = ipxlat_start_xmit,
};

static const struct device_type ipxlat_type = {
	.name = "ipxlat",
};

static void ipxlat_setup(struct net_device *dev)
{
	const netdev_features_t feat = NETIF_F_SG | NETIF_F_FRAGLIST |
				       NETIF_F_HW_CSUM | NETIF_F_HIGHDMA |
				       NETIF_F_GSO_SOFTWARE;

	dev->type = ARPHRD_NONE;
	dev->flags = IFF_NOARP;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->hard_header_len = 0;
	dev->addr_len = 0;

	dev->lltx = true;
	dev->features |= feat;
	dev->hw_features |= feat;
	dev->hw_enc_features |= feat;

	dev->netdev_ops = &ipxlat_netdev_ops;
	dev->needs_free_netdev = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
	dev->max_mtu = IP_MAX_MTU - sizeof(struct ipv6hdr) -
		       sizeof(struct iphdr);
	dev->min_mtu = IPV6_MIN_MTU;
	dev->mtu = ETH_DATA_LEN;

	/* keep skb->dst up to ndo_start_xmit so ICMP error emission can
	 * reuse routing metadata from ingress when available
	 */
	netif_keep_dst(dev);

	SET_NETDEV_DEVTYPE(dev, &ipxlat_type);
}

static struct rtnl_link_ops ipxlat_link_ops = {
	.kind = "ipxlat",
	.priv_size = sizeof(struct ipxlat_priv),
	.setup = ipxlat_setup,
};

bool ipxlat_dev_is_valid(const struct net_device *dev)
{
	return dev->rtnl_link_ops == &ipxlat_link_ops;
}

static int __init ipxlat_init(void)
{
	int err;

	err = rtnl_link_register(&ipxlat_link_ops);
	if (err) {
		pr_err("ipxlat: failed to register rtnl link ops: %d\n", err);
		return err;
	}

	return 0;
}

static void __exit ipxlat_exit(void)
{
	rtnl_link_unregister(&ipxlat_link_ops);
}

module_init(ipxlat_init);
module_exit(ipxlat_exit);
