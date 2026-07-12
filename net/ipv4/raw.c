// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		RAW - implementation of IP "raw" sockets.
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	verify_area() fixed up
 *		Alan Cox	:	ICMP error handling
 *		Alan Cox	:	EMSGSIZE if you send too big a packet
 *		Alan Cox	: 	Now uses generic datagrams and shared
 *					skbuff library. No more peek crashes,
 *					no more backlogs
 *		Alan Cox	:	Checks sk->broadcast.
 *		Alan Cox	:	Uses skb_free_datagram/skb_copy_datagram
 *		Alan Cox	:	Raw passes ip options too
 *		Alan Cox	:	Setsocketopt added
 *		Alan Cox	:	Fixed error return for broadcasts
 *		Alan Cox	:	Removed wake_up calls
 *		Alan Cox	:	Use ttl/tos
 *		Alan Cox	:	Cleaned up old debugging
 *		Alan Cox	:	Use new kernel side addresses
 *	Arnt Gulbrandsen	:	Fixed MSG_DONTROUTE in raw sockets.
 *		Alan Cox	:	BSD style RAW socket demultiplexing.
 *		Alan Cox	:	Beginnings of mrouted support.
 *		Alan Cox	:	Added IP_HDRINCL option.
 *		Alan Cox	:	Skip broadcast check if BSDism set.
 *		David S. Miller	:	New socket lookup architecture.
 */

#include <linux/types.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <asm/current.h>
#include <linux/uaccess.h>
#include <linux/stddef.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/sockios.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/in_route.h>
#include <linux/route.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/dst.h>
#include <net/sock.h>
#include <linux/ip.h>
#include <linux/net.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/raw.h>
#include <net/snmp.h>
#include <net/tcp_states.h>
#include <net/inet_common.h>
#include <net/checksum.h>
#include <linux/rtnetlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/compat.h>
#include <linux/uio.h>

struct raw_frag_vec {
	struct msghdr *msg;
	union {
		struct icmphdr icmph;
		char c[1];
	} hdr;
	int hlen;
};

struct raw_hashinfo raw_v4_hashinfo;
EXPORT_SYMBOL_GPL(raw_v4_hashinfo);

int raw_hash_sk(struct sock *sk)
{
	struct raw_hashinfo *h = sk->sk_prot->h.raw_hash;
	struct hlist_head *hlist;

	hlist = &h->ht[raw_hashfunc(sock_net(sk), inet_sk(sk)->inet_num)];

	spin_lock(&h->lock);
	sk_add_node_rcu(sk, hlist);
	sock_set_flag(sk, SOCK_RCU_FREE);
	spin_unlock(&h->lock);
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);

	return 0;
}

void raw_unhash_sk(struct sock *sk)
{
	struct raw_hashinfo *h = sk->sk_prot->h.raw_hash;

	spin_lock(&h->lock);
	if (sk_del_node_init_rcu(sk))
		sock_prot_inuse_add(sock_net(sk), sk->sk_prot, -1);
	spin_unlock(&h->lock);
}

int raw_abort(struct sock *sk, int err)
{
	lock_sock(sk);

	sk->sk_err = err;
	sk_error_report(sk);
	__udp_disconnect(sk, 0);

	release_sock(sk);

	return 0;
}

#ifdef CONFIG_PROC_FS
static struct sock *raw_get_first(struct seq_file *seq, int bucket)
{
	struct raw_hashinfo *h = pde_data(file_inode(seq->file));
	struct raw_iter_state *state = raw_seq_private(seq);
	struct hlist_head *hlist;
	struct sock *sk;

	for (state->bucket = bucket; state->bucket < RAW_HTABLE_SIZE;
			++state->bucket) {
		hlist = &h->ht[state->bucket];
		sk_for_each(sk, hlist) {
			if (sock_net(sk) == seq_file_net(seq))
				return sk;
		}
	}
	return NULL;
}

static struct sock *raw_get_next(struct seq_file *seq, struct sock *sk)
{
	struct raw_iter_state *state = raw_seq_private(seq);

	do {
		sk = sk_next(sk);
	} while (sk && sock_net(sk) != seq_file_net(seq));

	if (!sk)
		return raw_get_first(seq, state->bucket + 1);
	return sk;
}

static struct sock *raw_get_idx(struct seq_file *seq, loff_t pos)
{
	struct sock *sk = raw_get_first(seq, 0);

	if (sk)
		while (pos && (sk = raw_get_next(seq, sk)) != NULL)
			--pos;
	return pos ? NULL : sk;
}

void *raw_seq_start(struct seq_file *seq, loff_t *pos)
	__acquires(&h->lock)
{
	struct raw_hashinfo *h = pde_data(file_inode(seq->file));

	spin_lock(&h->lock);

	return *pos ? raw_get_idx(seq, *pos - 1) : SEQ_START_TOKEN;
}

void *raw_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct sock *sk;

	if (v == SEQ_START_TOKEN)
		sk = raw_get_first(seq, 0);
	else
		sk = raw_get_next(seq, v);
	++*pos;
	return sk;
}

void raw_seq_stop(struct seq_file *seq, void *v)
	__releases(&h->lock)
{
	struct raw_hashinfo *h = pde_data(file_inode(seq->file));

	spin_unlock(&h->lock);
}

static void raw_sock_seq_show(struct seq_file *seq, struct sock *sp, int i)
{
	struct inet_sock *inet = inet_sk(sp);
	__be32 dest = inet->inet_daddr,
	       src = inet->inet_rcv_saddr;
	__u16 destp = 0,
	      srcp  = inet->inet_num;

	seq_printf(seq, "%4d: %08X:%04X %08X:%04X"
		" %02X %08X:%08X %02X:%08lX %08X %5u %8d %llu %d %pK %u\n",
		i, src, srcp, dest, destp, sp->sk_state,
		sk_wmem_alloc_get(sp),
		sk_rmem_alloc_get(sp),
		0, 0L, 0,
		from_kuid_munged(seq_user_ns(seq), sk_uid(sp)),
		0, sock_i_ino(sp),
		refcount_read(&sp->sk_refcnt), sp, sk_drops_read(sp));
}

static int raw_seq_show(struct seq_file *seq, void *v)
{
	if (v == SEQ_START_TOKEN)
		seq_printf(seq, "  sl  local_address rem_address   st tx_queue "
				"rx_queue tr tm->when retrnsmt   uid  timeout "
				"inode ref pointer drops\n");
	else
		raw_sock_seq_show(seq, v, raw_seq_private(seq)->bucket);
	return 0;
}

static const struct seq_operations raw_seq_ops = {
	.start = raw_seq_start,
	.next  = raw_seq_next,
	.stop  = raw_seq_stop,
	.show  = raw_seq_show,
};

static __net_init int raw_init_net(struct net *net)
{
	if (!proc_create_net_data("raw", 0444, net->proc_net, &raw_seq_ops,
			sizeof(struct raw_iter_state), &raw_v4_hashinfo))
		return -ENOMEM;

	return 0;
}

static __net_exit void raw_exit_net(struct net *net)
{
	remove_proc_entry("raw", net->proc_net);
}

static __net_initdata struct pernet_operations raw_net_ops = {
	.init = raw_init_net,
	.exit = raw_exit_net,
};

int __init raw_proc_init(void)
{

	return register_pernet_subsys(&raw_net_ops);
}

void __init raw_proc_exit(void)
{
	unregister_pernet_subsys(&raw_net_ops);
}
#endif /* CONFIG_PROC_FS */

static void raw_sysctl_init_net(struct net *net)
{
#ifdef CONFIG_NET_L3_MASTER_DEV
	net->ipv4.sysctl_raw_l3mdev_accept = 1;
#endif
}

static int __net_init raw_sysctl_init(struct net *net)
{
	raw_sysctl_init_net(net);
	return 0;
}

static struct pernet_operations __net_initdata raw_sysctl_ops = {
	.init	= raw_sysctl_init,
};

void __init raw_init(void)
{
	raw_sysctl_init_net(&init_net);
	if (register_pernet_subsys(&raw_sysctl_ops))
		panic("RAW: failed to init sysctl parameters.\n");
}
