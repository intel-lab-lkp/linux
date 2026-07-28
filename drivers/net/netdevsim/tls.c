// SPDX-License-Identifier: GPL-2.0

#include <linux/ipv6.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/tls.h>

#include "netdevsim.h"

/* Emulated kTLS offload.  No crypto is performed, so this is only self
 * consistent between two netdevsim ports.
 */

#define NSIM_TLS_MAX_CONN	32

/* netdev_fix_features() drops NETIF_F_HW_TLS_RX unless the device also does
 * RX checksums, which every offload capable NIC does.
 */
#define NSIM_TLS_FEATURES	(NETIF_F_HW_TLS_TX | NETIF_F_HW_TLS_RX | \
				 NETIF_F_RXCSUM)

struct nsim_tls_conn {
	struct list_head list;
	struct rcu_head rcu;

	/* Identity as seen on the wire, from the point of view of the port
	 * the offload was installed on: l* is this side, r* is the peer.
	 */
	struct in6_addr laddr;
	struct in6_addr raddr;
	__be16 lport;
	__be16 rport;
	u16 family;

	enum tls_offload_ctx_dir dir;
	u32 start_sn;
	u16 cipher_type;
	const struct tls_context *tls_ctx;
};

struct nsim_tls_tuple {
	struct in6_addr saddr;
	struct in6_addr daddr;
	__be16 sport;
	__be16 dport;
};

static bool nsim_tls_parse(const struct sk_buff *skb,
			   struct nsim_tls_tuple *t)
{
	const struct tcphdr *th;

	switch (skb->protocol) {
	case htons(ETH_P_IP): {
		const struct iphdr *iph = ip_hdr(skb);

		if (iph->protocol != IPPROTO_TCP)
			return false;
		ipv6_addr_set_v4mapped(iph->saddr, &t->saddr);
		ipv6_addr_set_v4mapped(iph->daddr, &t->daddr);
		break;
	}
	case htons(ETH_P_IPV6): {
		const struct ipv6hdr *ip6h = ipv6_hdr(skb);

		if (ip6h->nexthdr != IPPROTO_TCP)
			return false;
		t->saddr = ip6h->saddr;
		t->daddr = ip6h->daddr;
		break;
	}
	default:
		return false;
	}

	th = tcp_hdr(skb);
	t->sport = th->source;
	t->dport = th->dest;

	return true;
}

/* On RX there is no socket to look at yet, so match the tuple installed at
 * ->tls_dev_add() time, which is what the hardware does.  The packet is
 * arriving, so the peer of the connection is its source.
 */
static bool nsim_tls_rx_offloaded(struct netdevsim *ns,
				  const struct nsim_tls_tuple *t)
{
	struct nsim_tls_conn *conn;

	list_for_each_entry_rcu(conn, &ns->tls.conns, list)
		if (conn->dir == TLS_OFFLOAD_CTX_DIR_RX &&
		    conn->lport == t->dport && conn->rport == t->sport &&
		    ipv6_addr_equal(&conn->laddr, &t->daddr) &&
		    ipv6_addr_equal(&conn->raddr, &t->saddr))
			return true;

	return false;
}

/* The stack leaves a tag sized placeholder at the end of every record for
 * the device to write the authentication tag into.  We do not encrypt, so
 * nothing ever fills it in, and it comes from a page frag that was never
 * zeroed - clear it rather than leak uninitialized memory onto the wire.
 *
 * tls_append_frag() either grows the last frag or adds one, so the tag is
 * always the tail of the record's last frag.  Clearing it there rather than
 * in the skb covers every segment the record was split into, and any
 * retransmit of them, since they all share these pages.
 */
static void nsim_tls_tx_zero_tags(struct sk_buff *skb)
{
	struct tls_context *ctx = tls_get_ctx(skb->sk);
	const struct tcphdr *th = tcp_hdr(skb);
	struct tls_offload_context_tx *tx_ctx;
	u32 seq, end, tag_size;
	unsigned long flags;
	unsigned int off;

	off = skb_transport_offset(skb) + __tcp_hdrlen(th);
	if (off >= skb->len)
		return;

	tag_size = ctx->prot_info.tag_size;
	seq = ntohl(th->seq);
	end = seq + skb->len - off;
	tx_ctx = tls_offload_ctx_tx(ctx);

	spin_lock_irqsave(&tx_ctx->lock, flags);
	while (before(seq, end)) {
		struct tls_record_info *record;
		skb_frag_t *frag;
		u64 rcd_sn;

		record = tls_get_record(tx_ctx, seq, &rcd_sn);
		if (!record || tls_record_is_start_marker(record))
			break;

		frag = &record->frags[record->num_frags - 1];
		memset(skb_frag_address(frag) + skb_frag_size(frag) - tag_size,
		       0, tag_size);

		seq = record->end_seq;
	}
	spin_unlock_irqrestore(&tx_ctx->lock, flags);
}

/* Stand in for the inline encryption the hardware would do on the way out.
 * The stack has already framed the record and we do not encrypt, so the tag
 * is all that is left to deal with.  The socket is right here, so identify
 * the connection the way the real drivers do.
 */
static void nsim_tls_tx(struct netdevsim *ns, struct sk_buff *skb)
{
	if (!tls_is_skb_tx_device_offloaded(skb))
		return;

	nsim_tls_tx_zero_tags(skb);
	atomic64_inc(&ns->tls.tx_packets);
	atomic64_add(skb->len, &ns->tls.tx_bytes);
}

/* Stand in for the inline decryption the peer's hardware would do, since we
 * are about to hand the skb to its stack.  We do not decrypt either, so all
 * that is needed is the flag that tells the kTLS core the payload is
 * already plaintext.
 */
static void nsim_tls_rx(struct netdevsim *ns, struct sk_buff *skb)
{
	struct nsim_tls_tuple t;

	if (list_empty(&ns->tls.conns))
		return;

	if (!nsim_tls_parse(skb, &t))
		return;

	if (!nsim_tls_rx_offloaded(ns, &t))
		return;

	skb->decrypted = 1;
	atomic64_inc(&ns->tls.rx_packets);
	atomic64_add(skb->len, &ns->tls.rx_bytes);
}

/* netdevsim has no wire: nsim_start_xmit() hands the skb straight to the
 * peer's receive path, so this one call site stands in for the hardware of
 * both ports.  It has to run before nsim_forward_skb() moves the skb over,
 * which resets the headers.
 */
void nsim_do_tls(struct sk_buff *skb, struct netdevsim *ns,
		 struct netdevsim *peer_ns)
{
	/* nsim_do_psp() ran first and may have wrapped the record stream, so
	 * this is no longer a plain TLS over TCP packet.  No NIC chains the
	 * two inline offloads either, so leave it alone.
	 */
	if (skb->encapsulation)
		return;

	nsim_tls_tx(ns, skb);
	nsim_tls_rx(peer_ns, skb);
}

static int nsim_tls_dev_add(struct net_device *netdev, struct sock *sk,
			    enum tls_offload_ctx_dir direction,
			    struct tls_crypto_info *crypto_info,
			    u32 start_offload_tcp_sn)
{
	struct netdevsim *ns = netdev_priv(netdev);
	struct nsim_tls_conn *conn;
	int ret = 0;

	/* Mirror the cipher support of a typical offload capable NIC. */
	switch (crypto_info->cipher_type) {
	case TLS_CIPHER_AES_GCM_128:
	case TLS_CIPHER_AES_GCM_256:
		break;
	default:
		return -EOPNOTSUPP;
	}

	conn = kzalloc_obj(*conn);
	if (!conn)
		return -ENOMEM;

	if (sk->sk_family == AF_INET6) {
		conn->laddr = sk->sk_v6_rcv_saddr;
		conn->raddr = sk->sk_v6_daddr;
	} else {
		ipv6_addr_set_v4mapped(sk->sk_rcv_saddr, &conn->laddr);
		ipv6_addr_set_v4mapped(sk->sk_daddr, &conn->raddr);
	}
	conn->family = sk->sk_family;
	conn->lport = htons(inet_sk(sk)->inet_num);
	conn->rport = sk->sk_dport;
	conn->dir = direction;
	conn->start_sn = start_offload_tcp_sn;
	conn->cipher_type = crypto_info->cipher_type;
	conn->tls_ctx = tls_get_ctx(sk);

	spin_lock_bh(&ns->tls.lock);
	if (ns->tls.count >= NSIM_TLS_MAX_CONN) {
		ret = -ENOSPC;
		goto out_unlock;
	}
	list_add_tail_rcu(&conn->list, &ns->tls.conns);
	ns->tls.count++;
	if (direction == TLS_OFFLOAD_CTX_DIR_TX)
		ns->tls.tx_conn++;
	else
		ns->tls.rx_conn++;
out_unlock:
	spin_unlock_bh(&ns->tls.lock);

	if (ret)
		kfree(conn);

	return ret;
}

static void nsim_tls_dev_del(struct net_device *netdev,
			     struct tls_context *tls_ctx,
			     enum tls_offload_ctx_dir direction)
{
	struct netdevsim *ns = netdev_priv(netdev);
	struct nsim_tls_conn *conn;

	spin_lock_bh(&ns->tls.lock);
	list_for_each_entry(conn, &ns->tls.conns, list) {
		if (conn->tls_ctx != tls_ctx || conn->dir != direction)
			continue;

		list_del_rcu(&conn->list);
		ns->tls.count--;
		if (direction == TLS_OFFLOAD_CTX_DIR_TX)
			ns->tls.tx_conn--;
		else
			ns->tls.rx_conn--;
		spin_unlock_bh(&ns->tls.lock);

		kfree_rcu(conn, rcu);
		return;
	}
	spin_unlock_bh(&ns->tls.lock);

	netdev_err(netdev, "TLS %s context not found on del\n",
		   direction == TLS_OFFLOAD_CTX_DIR_TX ? "tx" : "rx");
}

/* Nothing is ever out of sync here: the emulation does not decrypt, so it
 * never loses the record boundaries and never asks for a resync.  The core
 * can still call in on the RX path, so account for it and move on.
 */
static int nsim_tls_dev_resync(struct net_device *netdev, struct sock *sk,
			       u32 seq, u8 *rcd_sn,
			       enum tls_offload_ctx_dir direction)
{
	struct netdevsim *ns = netdev_priv(netdev);

	atomic64_inc(&ns->tls.resyncs);

	return 0;
}

static const struct tlsdev_ops nsim_tlsdev_ops = {
	.tls_dev_add	= nsim_tls_dev_add,
	.tls_dev_del	= nsim_tls_dev_del,
	.tls_dev_resync	= nsim_tls_dev_resync,
};

static ssize_t nsim_tls_dbg_read(struct file *filp, char __user *buffer,
				 size_t count, loff_t *ppos)
{
	struct netdevsim *ns = filp->private_data;
	struct nsim_tls_conn *conn;
	size_t bufsize;
	char *buf, *p;
	int len;

	/* Two full IPv6 addresses and ports fit in 160 bytes a line. */
	bufsize = (NSIM_TLS_MAX_CONN * 160) + 200;
	buf = kzalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	p = buf;

	spin_lock_bh(&ns->tls.lock);
	p += scnprintf(p, bufsize - (p - buf),
		       "conn count=%u tx=%u rx=%u\n",
		       ns->tls.count, ns->tls.tx_conn, ns->tls.rx_conn);
	p += scnprintf(p, bufsize - (p - buf),
		       "tx_packets=%llu tx_bytes=%llu rx_packets=%llu rx_bytes=%llu resyncs=%llu\n",
		       atomic64_read(&ns->tls.tx_packets),
		       atomic64_read(&ns->tls.tx_bytes),
		       atomic64_read(&ns->tls.rx_packets),
		       atomic64_read(&ns->tls.rx_bytes),
		       atomic64_read(&ns->tls.resyncs));

	list_for_each_entry(conn, &ns->tls.conns, list) {
		if (conn->family == AF_INET6)
			p += scnprintf(p, bufsize - (p - buf),
				       "%s [%pI6c]:%u -> [%pI6c]:%u cipher=%u sn=%u\n",
				       conn->dir == TLS_OFFLOAD_CTX_DIR_TX ?
				       "tx" : "rx",
				       &conn->laddr, ntohs(conn->lport),
				       &conn->raddr, ntohs(conn->rport),
				       conn->cipher_type, conn->start_sn);
		else
			p += scnprintf(p, bufsize - (p - buf),
				       "%s %pI4:%u -> %pI4:%u cipher=%u sn=%u\n",
				       conn->dir == TLS_OFFLOAD_CTX_DIR_TX ?
				       "tx" : "rx",
				       &conn->laddr.s6_addr32[3],
				       ntohs(conn->lport),
				       &conn->raddr.s6_addr32[3],
				       ntohs(conn->rport),
				       conn->cipher_type, conn->start_sn);
	}
	spin_unlock_bh(&ns->tls.lock);

	len = simple_read_from_buffer(buffer, count, ppos, buf, p - buf);

	kfree(buf);

	return len;
}

static const struct file_operations nsim_tls_dbg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = nsim_tls_dbg_read,
	.llseek = default_llseek,
};

void nsim_tls_init(struct netdevsim *ns)
{
	INIT_LIST_HEAD(&ns->tls.conns);
	spin_lock_init(&ns->tls.lock);

	ns->netdev->tlsdev_ops = &nsim_tlsdev_ops;
	ns->netdev->features |= NSIM_TLS_FEATURES;
	ns->netdev->hw_features |= NSIM_TLS_FEATURES;

	ns->tls.dfile = debugfs_create_file("tls", 0400,
					    ns->nsim_dev_port->ddir, ns,
					    &nsim_tls_dbg_fops);
}

void nsim_tls_teardown(struct netdevsim *ns)
{
	struct nsim_tls_conn *conn, *tmp;
	u32 left;

	debugfs_remove_recursive(ns->tls.dfile);

	spin_lock_bh(&ns->tls.lock);
	left = ns->tls.count;
	list_for_each_entry_safe(conn, tmp, &ns->tls.conns, list) {
		list_del_rcu(&conn->list);
		kfree_rcu(conn, rcu);
	}
	ns->tls.count = 0;
	ns->tls.tx_conn = 0;
	ns->tls.rx_conn = 0;
	spin_unlock_bh(&ns->tls.lock);

	if (left)
		netdev_err(ns->netdev,
			   "tearing down TLS offload with %u connections left\n",
			   left);
}
