// SPDX-License-Identifier: GPL-2.0

#include <linux/skbuff.h>
#include <net/psp.h>
#include <net/sock.h>

#include "netdevsim.h"

enum skb_drop_reason
nsim_psp_handle_tx(struct sk_buff *skb, struct netdevsim *ns)
{
	enum skb_drop_reason rc = 0;
	struct psp_assoc *pas;
	struct net *net;
	void **ptr;

	rcu_read_lock();
	pas = psp_skb_get_assoc_rcu(skb);
	if (!pas) {
		rc = SKB_NOT_DROPPED_YET;
		goto out_unlock;
	}

	if (!skb_transport_header_was_set(skb)) {
		rc = SKB_DROP_REASON_PSP_OUTPUT;
		goto out_unlock;
	}

	ptr = psp_assoc_drv_data(pas);
	if (*ptr != ns) {
		rc = SKB_DROP_REASON_PSP_OUTPUT;
		goto out_unlock;
	}

	net = sock_net(skb->sk);
	if (!psp_dev_encapsulate(net, skb, pas->tx.spi, pas->version, 0)) {
		rc = SKB_DROP_REASON_PSP_OUTPUT;
		goto out_unlock;
	}

	skb->decrypted = 0;

	u64_stats_update_begin(&ns->psp.syncp);
	u64_stats_inc(&ns->psp.tx_packets);
	u64_stats_add(&ns->psp.tx_bytes,
		      skb->len - skb_inner_transport_offset(skb));
	u64_stats_update_end(&ns->psp.syncp);
out_unlock:
	rcu_read_unlock();
	return rc;
}

/* Returns true if skb was consumed, false otherwise. */
bool nsim_psp_handle_rx(struct netdevsim *ns, struct sk_buff *skb)
{
	struct psp_dev *psd;
	struct psphdr *psph;
	struct udphdr *uh;
	int payload_len;
	u32 versions;
	int psp_off;
	bool is_udp;
	int l3_hlen;
	u8 version;
	u32 psd_id;
	int err;

	if (skb->protocol == htons(ETH_P_IP)) {
		struct iphdr *iph;

		if (!pskb_may_pull(skb, sizeof(struct iphdr)))
			return false;

		iph = (struct iphdr *)skb->data;
		if (iph->ihl < 5)
			return false;

		is_udp = iph->protocol == IPPROTO_UDP;
		l3_hlen = iph->ihl * 4;
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		struct ipv6hdr *ip6h;

		if (!pskb_may_pull(skb, sizeof(struct ipv6hdr)))
			return false;
		ip6h = (struct ipv6hdr *)skb->data;
		is_udp = ip6h->nexthdr == IPPROTO_UDP;
		l3_hlen = sizeof(struct ipv6hdr);
	} else {
		return false;
	}

	if (!is_udp)
		return false;

	if (!pskb_may_pull(skb, l3_hlen + sizeof(struct udphdr) + PSP_HDR_SIZE))
		return false;

	uh = (struct udphdr *)(skb->data + l3_hlen);
	if (uh->dest != htons(PSP_DEFAULT_UDP_PORT))
		return false;

	psph = (struct psphdr *)(uh + 1);
	version = FIELD_GET(PSPHDR_VERFL_VERSION, psph->verfl);

	rcu_read_lock();
	psd = rcu_dereference(ns->psp.dev);
	if (psd) {
		versions = READ_ONCE(psd->config.versions);
		psd_id = psd->id;
	}
	rcu_read_unlock();

	if (!psd || !(versions & (1 << version))) {
		skb->ip_summed = CHECKSUM_NONE;
		return false;
	}

	psp_off = l3_hlen + sizeof(struct udphdr);
	payload_len = skb->len - psp_off - PSP_HDR_SIZE - PSP_TRL_SIZE;
	if (payload_len < 0)
		goto drop;

	skb_push(skb, ETH_HLEN);
	skb->mac_len = ETH_HLEN;
	err = psp_dev_rcv(skb, psd_id, 0, false);
	if (err)
		goto drop;

	skb_reset_mac_header(skb);
	skb_pull(skb, ETH_HLEN);
	skb->decrypted = 1;

	u64_stats_update_begin(&ns->psp.syncp);
	u64_stats_inc(&ns->psp.rx_packets);
	u64_stats_add(&ns->psp.rx_bytes, payload_len);
	u64_stats_update_end(&ns->psp.syncp);

	return false;

drop:
	kfree_skb_reason(skb, SKB_DROP_REASON_PSP_INPUT);
	return true;
}

static int
nsim_psp_set_config(struct psp_dev *psd, struct psp_dev_config *conf,
		    struct netlink_ext_ack *extack)
{
	return 0;
}

static int
nsim_rx_spi_alloc(struct psp_dev *psd, u32 version,
		  struct psp_key_parsed *assoc,
		  struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = psd->drv_priv;
	int i;

	if ((ns->psp.spi ^ (ns->psp.spi + 1)) & PSP_SPI_KEY_PHASE) {
		NL_SET_ERR_MSG(extack, "SPI space exhausted");
		return -ENOSPC;
	}

	assoc->spi = cpu_to_be32(++ns->psp.spi);
	assoc->key[0] = psd->generation;
	for (i = 1; i < PSP_MAX_KEY; i++)
		assoc->key[i] = ns->psp.spi + i;

	return 0;
}

static int nsim_assoc_add(struct psp_dev *psd, struct psp_assoc *pas,
			  struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = psd->drv_priv;
	void **ptr = psp_assoc_drv_data(pas);

	/* Copy drv_priv from psd to assoc */
	*ptr = psd->drv_priv;
	ns->psp.assoc_cnt++;

	return 0;
}

static int nsim_key_rotate(struct psp_dev *psd, struct netlink_ext_ack *extack)
{
	struct netdevsim *ns = psd->drv_priv;

	ns->psp.spi = (ns->psp.spi & PSP_SPI_KEY_PHASE) ^ PSP_SPI_KEY_PHASE;

	return 0;
}

static void nsim_assoc_del(struct psp_dev *psd, struct psp_assoc *pas)
{
	struct netdevsim *ns = psd->drv_priv;
	void **ptr = psp_assoc_drv_data(pas);

	*ptr = NULL;
	ns->psp.assoc_cnt--;
}

static void nsim_get_stats(struct psp_dev *psd, struct psp_dev_stats *stats)
{
	struct netdevsim *ns = psd->drv_priv;
	unsigned int start;

	/* WARNING: do *not* blindly zero stats in real drivers!
	 * All required stats must be reported by the device!
	 */
	memset(stats, 0, sizeof(struct psp_dev_stats));

	do {
		start = u64_stats_fetch_begin(&ns->psp.syncp);
		stats->rx_bytes = u64_stats_read(&ns->psp.rx_bytes);
		stats->rx_packets = u64_stats_read(&ns->psp.rx_packets);
		stats->tx_bytes = u64_stats_read(&ns->psp.tx_bytes);
		stats->tx_packets = u64_stats_read(&ns->psp.tx_packets);
	} while (u64_stats_fetch_retry(&ns->psp.syncp, start));
}

static struct psp_dev_ops nsim_psp_ops = {
	.set_config	= nsim_psp_set_config,
	.rx_spi_alloc	= nsim_rx_spi_alloc,
	.tx_key_add	= nsim_assoc_add,
	.tx_key_del	= nsim_assoc_del,
	.key_rotate	= nsim_key_rotate,
	.get_stats	= nsim_get_stats,
};

static struct psp_dev_caps nsim_psp_caps = {
	.versions = 1 << PSP_VERSION_HDR0_AES_GCM_128 |
		    1 << PSP_VERSION_HDR0_AES_GMAC_128 |
		    1 << PSP_VERSION_HDR0_AES_GCM_256 |
		    1 << PSP_VERSION_HDR0_AES_GMAC_256,
	.assoc_drv_spc = sizeof(void *),
};

static void __nsim_psp_uninit(struct netdevsim *ns, bool teardown)
{
	struct psp_dev *psd;

	psd = rcu_dereference_protected(ns->psp.dev,
					teardown ||
					lockdep_is_held(&ns->psp.rereg_lock));
	if (psd) {
		rcu_assign_pointer(ns->psp.dev, NULL);
		synchronize_rcu();
		psp_dev_unregister(psd);
	}
	WARN_ON(ns->psp.assoc_cnt);
}

void nsim_psp_uninit(struct netdevsim *ns)
{
	debugfs_remove(ns->psp.rereg);
	mutex_destroy(&ns->psp.rereg_lock);
	__nsim_psp_uninit(ns, true);
}

static ssize_t
nsim_psp_rereg_write(struct file *file, const char __user *data, size_t count,
		     loff_t *ppos)
{
	struct netdevsim *ns = file->private_data;
	struct psp_dev *psd;
	ssize_t ret;

	mutex_lock(&ns->psp.rereg_lock);
	__nsim_psp_uninit(ns, false);

	psd = psp_dev_create(ns->netdev, &nsim_psp_ops, &nsim_psp_caps, ns);
	if (IS_ERR(psd)) {
		ret = PTR_ERR(psd);
		goto out;
	}

	rcu_assign_pointer(ns->psp.dev, psd);
	ret = count;
out:
	mutex_unlock(&ns->psp.rereg_lock);
	return ret;
}

static const struct file_operations nsim_psp_rereg_fops = {
	.open = simple_open,
	.write = nsim_psp_rereg_write,
	.llseek = generic_file_llseek,
	.owner = THIS_MODULE,
};

int nsim_psp_init(struct netdevsim *ns)
{
	struct dentry *ddir = ns->nsim_dev_port->ddir;
	struct psp_dev *psd;

	psd = psp_dev_create(ns->netdev, &nsim_psp_ops, &nsim_psp_caps, ns);
	if (IS_ERR(psd))
		return PTR_ERR(psd);

	rcu_assign_pointer(ns->psp.dev, psd);

	mutex_init(&ns->psp.rereg_lock);
	ns->psp.rereg = debugfs_create_file("psp_rereg", 0200, ddir, ns,
					    &nsim_psp_rereg_fops);
	return 0;
}
