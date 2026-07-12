// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		The User Datagram Protocol (UDP).
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Alan Cox, <alan@lxorguk.ukuu.org.uk>
 *		Hirokazu Takahashi, <taka@valinux.co.jp>
 *
 * Fixes:
 *		Alan Cox	:	verify_area() calls
 *		Alan Cox	: 	stopped close while in use off icmp
 *					messages. Not a fix but a botch that
 *					for udp at least is 'valid'.
 *		Alan Cox	:	Fixed icmp handling properly
 *		Alan Cox	: 	Correct error for oversized datagrams
 *		Alan Cox	:	Tidied select() semantics.
 *		Alan Cox	:	udp_err() fixed properly, also now
 *					select and read wake correctly on errors
 *		Alan Cox	:	udp_send verify_area moved to avoid mem leak
 *		Alan Cox	:	UDP can count its memory
 *		Alan Cox	:	send to an unknown connection causes
 *					an ECONNREFUSED off the icmp, but
 *					does NOT close.
 *		Alan Cox	:	Switched to new sk_buff handlers. No more backlog!
 *		Alan Cox	:	Using generic datagram code. Even smaller and the PEEK
 *					bug no longer crashes it.
 *		Fred Van Kempen	: 	Net2e support for sk->broadcast.
 *		Alan Cox	:	Uses skb_free_datagram
 *		Alan Cox	:	Added get/set sockopt support.
 *		Alan Cox	:	Broadcasting without option set returns EACCES.
 *		Alan Cox	:	No wakeup calls. Instead we now use the callbacks.
 *		Alan Cox	:	Use ip_tos and ip_ttl
 *		Alan Cox	:	SNMP Mibs
 *		Alan Cox	:	MSG_DONTROUTE, and 0.0.0.0 support.
 *		Matt Dillon	:	UDP length checks.
 *		Alan Cox	:	Smarter af_inet used properly.
 *		Alan Cox	:	Use new kernel side addressing.
 *		Alan Cox	:	Incorrect return on truncated datagram receive.
 *	Arnt Gulbrandsen 	:	New udp_send and stuff
 *		Alan Cox	:	Cache last socket
 *		Alan Cox	:	Route cache
 *		Jon Peatfield	:	Minor efficiency fix to sendto().
 *		Mike Shaver	:	RFC1122 checks.
 *		Alan Cox	:	Nonblocking error fix.
 *	Willy Konynenberg	:	Transparent proxying support.
 *		Mike McLagan	:	Routing by source
 *		David S. Miller	:	New socket lookup architecture.
 *					Last socket cache retained as it
 *					does have a high hit rate.
 *		Olaf Kirch	:	Don't linearise iovec on sendmsg.
 *		Andi Kleen	:	Some cleanups, cache destination entry
 *					for connect.
 *	Vitaly E. Lavrov	:	Transparent proxy revived after year coma.
 *		Melvin Smith	:	Check msg_name not msg_namelen in sendto(),
 *					return ENOTCONN for unconnected sockets (POSIX)
 *		Janos Farkas	:	don't deliver multi/broadcasts to a different
 *					bound-to-device socket
 *	Hirokazu Takahashi	:	HW checksumming for outgoing UDP
 *					datagrams.
 *	Hirokazu Takahashi	:	sendfile() on UDP works now.
 *		Arnaldo C. Melo :	convert /proc/net/udp to seq_file
 *	YOSHIFUJI Hideaki @USAGI and:	Support IPV6_V6ONLY socket option, which
 *	Alexey Kuznetsov:		allow both IPv4 and IPv6 sockets to bind
 *					a single port at the same time.
 *	Derek Atkins <derek@ihtfp.com>: Add Encapsulation Support
 *	James Chapman		:	Add L2TP encapsulation type.
 */

#define pr_fmt(fmt) "UDP: " fmt

#include <linux/bpf-cgroup.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <asm/ioctls.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/igmp.h>
#include <linux/inetdevice.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/sock_diag.h>
#include <net/tcp_states.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/aligned_data.h>
#include <net/net_namespace.h>
#include <net/icmp.h>
#include <net/inet_common.h>
#include <net/inet_hashtables.h>
#include <net/ip.h>
#include <net/ip_tunnels.h>
#include <net/route.h>
#include <net/checksum.h>
#include <net/gso.h>
#include <net/xfrm.h>
#include <trace/events/udp.h>
#include <linux/static_key.h>
#include <linux/btf_ids.h>
#include <trace/events/skb.h>
#include <net/busy_poll.h>
#include <net/sock_reuseport.h>
#include <net/addrconf.h>
#include <net/udp_tunnel.h>
#include <net/gro.h>
#include <net/rps.h>

struct udp_table udp_table __read_mostly;

long sysctl_udp_mem[3] __read_mostly;

DEFINE_PER_CPU(int, udp_memory_per_cpu_fw_alloc);
EXPORT_PER_CPU_SYMBOL_GPL(udp_memory_per_cpu_fw_alloc);

#define MAX_UDP_PORTS 65536
#define PORTS_PER_CHAIN (MAX_UDP_PORTS / UDP_HTABLE_SIZE_MIN_PERNET)

static int udp_lib_lport_inuse(struct net *net, __u16 num,
			       const struct udp_hslot *hslot,
			       unsigned long *bitmap,
			       struct sock *sk, unsigned int log)
{
	kuid_t uid = sk_uid(sk);
	struct sock *sk2;

	sk_for_each(sk2, &hslot->head) {
		if (net_eq(sock_net(sk2), net) &&
		    sk2 != sk &&
		    (bitmap || udp_sk(sk2)->udp_port_hash == num) &&
		    (!sk2->sk_reuse || !sk->sk_reuse) &&
		    (!sk2->sk_bound_dev_if || !sk->sk_bound_dev_if ||
		     sk2->sk_bound_dev_if == sk->sk_bound_dev_if) &&
		    inet_rcv_saddr_equal(sk, sk2, true)) {
			if (sk2->sk_reuseport && sk->sk_reuseport &&
			    !rcu_access_pointer(sk->sk_reuseport_cb) &&
			    uid_eq(uid, sk_uid(sk2))) {
				if (!bitmap)
					return 0;
			} else {
				if (!bitmap)
					return 1;
				__set_bit(udp_sk(sk2)->udp_port_hash >> log,
					  bitmap);
			}
		}
	}
	return 0;
}

/*
 * Note: we still hold spinlock of primary hash chain, so no other writer
 * can insert/delete a socket with local_port == num
 */
static int udp_lib_lport_inuse2(struct net *net, __u16 num,
				struct udp_hslot *hslot2,
				struct sock *sk)
{
	kuid_t uid = sk_uid(sk);
	struct sock *sk2;
	int res = 0;

	spin_lock(&hslot2->lock);
	udp_portaddr_for_each_entry(sk2, &hslot2->head) {
		if (net_eq(sock_net(sk2), net) &&
		    sk2 != sk &&
		    (udp_sk(sk2)->udp_port_hash == num) &&
		    (!sk2->sk_reuse || !sk->sk_reuse) &&
		    (!sk2->sk_bound_dev_if || !sk->sk_bound_dev_if ||
		     sk2->sk_bound_dev_if == sk->sk_bound_dev_if) &&
		    inet_rcv_saddr_equal(sk, sk2, true)) {
			if (sk2->sk_reuseport && sk->sk_reuseport &&
			    !rcu_access_pointer(sk->sk_reuseport_cb) &&
			    uid_eq(uid, sk_uid(sk2))) {
				res = 0;
			} else {
				res = 1;
			}
			break;
		}
	}
	spin_unlock(&hslot2->lock);
	return res;
}

static int udp_reuseport_add_sock(struct sock *sk, struct udp_hslot *hslot)
{
	struct net *net = sock_net(sk);
	kuid_t uid = sk_uid(sk);
	struct sock *sk2;

	sk_for_each(sk2, &hslot->head) {
		if (net_eq(sock_net(sk2), net) &&
		    sk2 != sk &&
		    sk2->sk_family == sk->sk_family &&
		    ipv6_only_sock(sk2) == ipv6_only_sock(sk) &&
		    (udp_sk(sk2)->udp_port_hash == udp_sk(sk)->udp_port_hash) &&
		    (sk2->sk_bound_dev_if == sk->sk_bound_dev_if) &&
		    sk2->sk_reuseport && uid_eq(uid, sk_uid(sk2)) &&
		    inet_rcv_saddr_equal(sk, sk2, false)) {
			return reuseport_add_sock(sk, sk2,
						  inet_rcv_saddr_any(sk));
		}
	}

	return reuseport_alloc(sk, inet_rcv_saddr_any(sk));
}

/**
 *  udp_lib_get_port  -  UDP port lookup for IPv4 and IPv6
 *
 *  @sk:          socket struct in question
 *  @snum:        port number to look up
 *  @hash2_nulladdr: AF-dependent hash value in secondary hash chains,
 *                   with NULL address
 */
int udp_lib_get_port(struct sock *sk, unsigned short snum,
		     unsigned int hash2_nulladdr)
{
	struct udp_hslot *hslot, *hslot2;
	struct net *net = sock_net(sk);
	struct udp_table *udptable;
	int error = -EADDRINUSE;

	udptable = net->ipv4.udp_table;

	if (!snum) {
		DECLARE_BITMAP(bitmap, PORTS_PER_CHAIN);
		unsigned short first, last;
		int low, high, remaining;
		unsigned int rand;

		inet_sk_get_local_port_range(sk, &low, &high);
		remaining = (high - low) + 1;

		rand = get_random_u32();
		first = reciprocal_scale(rand, remaining) + low;
		/*
		 * force rand to be an odd multiple of UDP_HTABLE_SIZE
		 */
		rand = (rand | 1) * (udptable->mask + 1);
		last = first + udptable->mask + 1;
		do {
			hslot = udp_hashslot(udptable, net, first);
			bitmap_zero(bitmap, PORTS_PER_CHAIN);
			spin_lock_bh(&hslot->lock);
			udp_lib_lport_inuse(net, snum, hslot, bitmap, sk,
					    udptable->log);

			snum = first;
			/*
			 * Iterate on all possible values of snum for this hash.
			 * Using steps of an odd multiple of UDP_HTABLE_SIZE
			 * give us randomization and full range coverage.
			 */
			do {
				if (low <= snum && snum <= high &&
				    !test_bit(snum >> udptable->log, bitmap) &&
				    !inet_is_local_reserved_port(net, snum))
					goto found;
				snum += rand;
			} while (snum != first);
			spin_unlock_bh(&hslot->lock);
			cond_resched();
		} while (++first != last);
		goto fail;
	} else {
		hslot = udp_hashslot(udptable, net, snum);
		spin_lock_bh(&hslot->lock);
		if (inet_use_hash2_on_bind(sk) && hslot->count > 10) {
			int exist;
			unsigned int slot2 = udp_sk(sk)->udp_portaddr_hash ^ snum;

			slot2          &= udptable->mask;
			hash2_nulladdr &= udptable->mask;

			hslot2 = udp_hashslot2(udptable, slot2);
			if (hslot->count < hslot2->count)
				goto scan_primary_hash;

			exist = udp_lib_lport_inuse2(net, snum, hslot2, sk);
			if (!exist && (hash2_nulladdr != slot2)) {
				hslot2 = udp_hashslot2(udptable, hash2_nulladdr);
				exist = udp_lib_lport_inuse2(net, snum, hslot2,
							     sk);
			}
			if (exist)
				goto fail_unlock;
			else
				goto found;
		}
scan_primary_hash:
		if (udp_lib_lport_inuse(net, snum, hslot, NULL, sk, 0))
			goto fail_unlock;
	}
found:
	inet_sk(sk)->inet_num = snum;
	udp_sk(sk)->udp_port_hash = snum;
	udp_sk(sk)->udp_portaddr_hash ^= snum;
	if (sk_unhashed(sk)) {
		if (sk->sk_reuseport &&
		    udp_reuseport_add_sock(sk, hslot)) {
			inet_sk(sk)->inet_num = 0;
			udp_sk(sk)->udp_port_hash = 0;
			udp_sk(sk)->udp_portaddr_hash ^= snum;
			goto fail_unlock;
		}

		sock_set_flag(sk, SOCK_RCU_FREE);

		sk_add_node_rcu(sk, &hslot->head);
		hslot->count++;
		sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);

		hslot2 = udp_hashslot2(udptable, udp_sk(sk)->udp_portaddr_hash);
		spin_lock(&hslot2->lock);
		if (IS_ENABLED(CONFIG_IPV6) && sk->sk_reuseport &&
		    sk->sk_family == AF_INET6)
			hlist_add_tail_rcu(&udp_sk(sk)->udp_portaddr_node,
					   &hslot2->head);
		else
			hlist_add_head_rcu(&udp_sk(sk)->udp_portaddr_node,
					   &hslot2->head);
		hslot2->count++;
		spin_unlock(&hslot2->lock);
	}

	error = 0;
fail_unlock:
	spin_unlock_bh(&hslot->lock);
fail:
	return error;
}

u32 udp_ehashfn(const struct net *net, const __be32 laddr, const __u16 lport,
		const __be32 faddr, const __be16 fport)
{
	net_get_random_once(&udp_ehash_secret, sizeof(udp_ehash_secret));

	return __inet_ehashfn(laddr, lport, faddr, fport,
			      udp_ehash_secret + net_hash_mix(net));
}

#if IS_ENABLED(CONFIG_BASE_SMALL)
static void udp_rehash4(struct udp_table *udptable, struct sock *sk,
			u16 newhash4)
{
}

static void udp_unhash4(struct udp_table *udptable, struct sock *sk)
{
}
#else /* !CONFIG_BASE_SMALL */
/* udp_rehash4() only checks hslot4, and hash4_cnt is not processed. */
static void udp_rehash4(struct udp_table *udptable, struct sock *sk,
			u16 newhash4)
{
	struct udp_hslot *hslot4, *nhslot4;

	hslot4 = udp_hashslot4(udptable, udp_sk(sk)->udp_lrpa_hash);
	nhslot4 = udp_hashslot4(udptable, newhash4);
	udp_sk(sk)->udp_lrpa_hash = newhash4;

	if (hslot4 != nhslot4) {
		spin_lock_bh(&hslot4->lock);
		hlist_nulls_del_init_rcu(&udp_sk(sk)->udp_lrpa_node);
		hslot4->count--;
		spin_unlock_bh(&hslot4->lock);

		spin_lock_bh(&nhslot4->lock);
		hlist_nulls_add_head_rcu(&udp_sk(sk)->udp_lrpa_node,
					 &nhslot4->nulls_head);
		nhslot4->count++;
		spin_unlock_bh(&nhslot4->lock);
	}
}

static void udp_unhash4(struct udp_table *udptable, struct sock *sk)
{
	struct udp_hslot *hslot2, *hslot4;

	if (udp_hashed4(sk)) {
		hslot2 = udp_hashslot2(udptable, udp_sk(sk)->udp_portaddr_hash);
		hslot4 = udp_hashslot4(udptable, udp_sk(sk)->udp_lrpa_hash);

		spin_lock(&hslot4->lock);
		hlist_nulls_del_init_rcu(&udp_sk(sk)->udp_lrpa_node);
		hslot4->count--;
		spin_unlock(&hslot4->lock);

		spin_lock(&hslot2->lock);
		udp_hash4_dec(hslot2);
		spin_unlock(&hslot2->lock);
	}
}

void udp_lib_hash4(struct sock *sk, u16 hash)
{
	struct udp_hslot *hslot, *hslot2, *hslot4;
	struct net *net = sock_net(sk);
	struct udp_table *udptable;

	/* Connected udp socket can re-connect to another remote address, which
	 * will be handled by rehash. Thus no need to redo hash4 here.
	 */
	if (udp_hashed4(sk))
		return;

	udptable = net->ipv4.udp_table;
	hslot = udp_hashslot(udptable, net, udp_sk(sk)->udp_port_hash);
	hslot2 = udp_hashslot2(udptable, udp_sk(sk)->udp_portaddr_hash);
	hslot4 = udp_hashslot4(udptable, hash);
	udp_sk(sk)->udp_lrpa_hash = hash;

	spin_lock_bh(&hslot->lock);
	if (rcu_access_pointer(sk->sk_reuseport_cb))
		reuseport_detach_sock(sk);

	spin_lock(&hslot4->lock);
	hlist_nulls_add_head_rcu(&udp_sk(sk)->udp_lrpa_node,
				 &hslot4->nulls_head);
	hslot4->count++;
	spin_unlock(&hslot4->lock);

	spin_lock(&hslot2->lock);
	udp_hash4_inc(hslot2);
	spin_unlock(&hslot2->lock);

	spin_unlock_bh(&hslot->lock);
}
#endif /* CONFIG_BASE_SMALL */

DEFINE_STATIC_KEY_FALSE(udp_encap_needed_key);

#if IS_ENABLED(CONFIG_IPV6)
DEFINE_STATIC_KEY_FALSE(udpv6_encap_needed_key);
#endif

void udp_encap_enable(void)
{
	static_branch_inc(&udp_encap_needed_key);
}
EXPORT_SYMBOL(udp_encap_enable);

void udp_encap_disable(void)
{
	static_branch_dec(&udp_encap_needed_key);
}
EXPORT_SYMBOL(udp_encap_disable);

/*
 * Throw away all pending data and cancel the corking. Socket is locked.
 */
void udp_flush_pending_frames(struct sock *sk)
{
	struct udp_sock *up = udp_sk(sk);

	if (up->pending) {
		up->len = 0;
		WRITE_ONCE(up->pending, 0);
		ip_flush_pending_frames(sk);
	}
}

/* Function to set UDP checksum for an IPv4 UDP packet. This is intended
 * for the simple case like when setting the checksum for a UDP tunnel.
 */
void udp_set_csum(bool nocheck, struct sk_buff *skb,
		  __be32 saddr, __be32 daddr, int len)
{
	struct udphdr *uh = udp_hdr(skb);

	if (nocheck) {
		uh->check = 0;
	} else if (skb_is_gso(skb)) {
		uh->check = ~udp_v4_check(len, saddr, daddr, 0);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		uh->check = 0;
		uh->check = udp_v4_check(len, saddr, daddr, lco_csum(skb));
		if (uh->check == 0)
			uh->check = CSUM_MANGLED_0;
	} else {
		skb->ip_summed = CHECKSUM_PARTIAL;
		skb->csum_start = skb_transport_header(skb) - skb->head;
		skb->csum_offset = offsetof(struct udphdr, check);
		uh->check = ~udp_v4_check(len, saddr, daddr, 0);
	}
}
EXPORT_SYMBOL(udp_set_csum);

static int __udp_cmsg_send(struct cmsghdr *cmsg, u16 *gso_size)
{
	switch (cmsg->cmsg_type) {
	case UDP_SEGMENT:
		if (cmsg->cmsg_len != CMSG_LEN(sizeof(__u16)))
			return -EINVAL;
		*gso_size = *(__u16 *)CMSG_DATA(cmsg);
		return 0;
	default:
		return -EINVAL;
	}
}

int udp_cmsg_send(struct sock *sk, struct msghdr *msg, u16 *gso_size)
{
	struct cmsghdr *cmsg;
	bool need_ip = false;
	int err;

	for_each_cmsghdr(cmsg, msg) {
		if (!CMSG_OK(msg, cmsg))
			return -EINVAL;

		if (cmsg->cmsg_level != SOL_UDP) {
			need_ip = true;
			continue;
		}

		err = __udp_cmsg_send(cmsg, gso_size);
		if (err)
			return err;
	}

	return need_ip;
}

#define UDP_SKB_IS_STATELESS 0x80000000

/* all head states (dst, sk, nf conntrack) except skb extensions are
 * cleared by udp_rcv().
 *
 * We need to preserve secpath, if present, to eventually process
 * IP_CMSG_PASSSEC at recvmsg() time.
 *
 * Other extensions can be cleared.
 */
static bool udp_try_make_stateless(struct sk_buff *skb)
{
	if (!skb_has_extensions(skb))
		return true;

	if (!secpath_exists(skb)) {
		skb_ext_reset(skb);
		return true;
	}

	return false;
}

static void udp_set_dev_scratch(struct sk_buff *skb)
{
	struct udp_dev_scratch *scratch = udp_skb_scratch(skb);

	BUILD_BUG_ON(sizeof(struct udp_dev_scratch) > sizeof(long));
	scratch->_tsize_state = skb->truesize;
#if BITS_PER_LONG == 64
	scratch->len = skb->len;
	scratch->csum_unnecessary = !!skb_csum_unnecessary(skb);
	scratch->is_linear = !skb_is_nonlinear(skb);
#endif
	if (udp_try_make_stateless(skb))
		scratch->_tsize_state |= UDP_SKB_IS_STATELESS;
}

static void udp_skb_csum_unnecessary_set(struct sk_buff *skb)
{
	/* We come here after udp_lib_checksum_complete() returned 0.
	 * This means that __skb_checksum_complete() might have
	 * set skb->csum_valid to 1.
	 * On 64bit platforms, we can set csum_unnecessary
	 * to true, but only if the skb is not shared.
	 */
#if BITS_PER_LONG == 64
	if (!skb_shared(skb))
		udp_skb_scratch(skb)->csum_unnecessary = true;
#endif
}

static int udp_skb_truesize(struct sk_buff *skb)
{
	return udp_skb_scratch(skb)->_tsize_state & ~UDP_SKB_IS_STATELESS;
}

static bool udp_skb_has_head_state(struct sk_buff *skb)
{
	return !(udp_skb_scratch(skb)->_tsize_state & UDP_SKB_IS_STATELESS);
}

/* fully reclaim rmem/fwd memory allocated for skb */
static void udp_rmem_release(struct sock *sk, unsigned int size,
			     int partial, bool rx_queue_lock_held)
{
	struct udp_sock *up = udp_sk(sk);
	struct sk_buff_head *sk_queue;
	unsigned int amt;

	if (likely(partial)) {
		up->forward_deficit += size;
		size = up->forward_deficit;
		if (size < READ_ONCE(up->forward_threshold) &&
		    !skb_queue_empty(&up->reader_queue))
			return;
	} else {
		size += up->forward_deficit;
	}
	up->forward_deficit = 0;

	/* acquire the sk_receive_queue for fwd allocated memory scheduling,
	 * if the called don't held it already
	 */
	sk_queue = &sk->sk_receive_queue;
	if (!rx_queue_lock_held)
		spin_lock(&sk_queue->lock);

	amt = (size + sk->sk_forward_alloc - partial) & ~(PAGE_SIZE - 1);
	sk_forward_alloc_add(sk, size - amt);

	if (amt)
		__sk_mem_reduce_allocated(sk, amt >> PAGE_SHIFT);

	atomic_sub(size, &sk->sk_rmem_alloc);

	/* this can save us from acquiring the rx queue lock on next receive */
	skb_queue_splice_tail_init(sk_queue, &up->reader_queue);

	if (!rx_queue_lock_held)
		spin_unlock(&sk_queue->lock);
}

/* Note: called with reader_queue.lock held.
 * Instead of using skb->truesize here, find a copy of it in skb->dev_scratch
 * This avoids a cache line miss while receive_queue lock is held.
 * Look at __udp_enqueue_schedule_skb() to find where this copy is done.
 */
void udp_skb_destructor(struct sock *sk, struct sk_buff *skb)
{
	prefetch(&skb->data);
	udp_rmem_release(sk, udp_skb_truesize(skb), 1, false);
}

/* as above, but the caller held the rx queue lock, too */
static void udp_skb_dtor_locked(struct sock *sk, struct sk_buff *skb)
{
	prefetch(&skb->data);
	udp_rmem_release(sk, udp_skb_truesize(skb), 1, true);
}

static int udp_rmem_schedule(struct sock *sk, int size)
{
	int delta;

	delta = size - sk->sk_forward_alloc;
	if (delta > 0 && !__sk_mem_schedule(sk, delta, SK_MEM_RECV))
		return -ENOBUFS;

	return 0;
}

int __udp_enqueue_schedule_skb(struct sock *sk, struct sk_buff *skb)
{
	struct sk_buff_head *list = &sk->sk_receive_queue;
	struct udp_prod_queue *udp_prod_queue;
	struct sk_buff *next, *to_drop = NULL;
	struct llist_node *ll_list;
	unsigned int rmem, rcvbuf;
	int size, err = -ENOMEM;
	int total_size = 0;
	int q_size = 0;
	int dropcount;
	int nb = 0;

	rmem = atomic_read(&sk->sk_rmem_alloc);
	rcvbuf = READ_ONCE(sk->sk_rcvbuf);
	size = skb->truesize;

	udp_prod_queue = &udp_sk(sk)->udp_prod_queue[numa_node_id()];

	rmem += atomic_read(&udp_prod_queue->rmem_alloc);

	/* Immediately drop when the receive queue is full.
	 * Cast to unsigned int performs the boundary check for INT_MAX.
	 */
	if (rmem + size > rcvbuf) {
		if (rcvbuf > INT_MAX >> 1)
			goto drop;

		/* Accept the packet if queue is empty. */
		if (rmem)
			goto drop;
	}

	/* Under mem pressure, it might be helpful to help udp_recvmsg()
	 * having linear skbs :
	 * - Reduce memory overhead and thus increase receive queue capacity
	 * - Less cache line misses at copyout() time
	 * - Less work at consume_skb() (less alien page frag freeing)
	 */
	if (rmem > (rcvbuf >> 1)) {
		skb_condense(skb);
		size = skb->truesize;
	}

	udp_set_dev_scratch(skb);

	atomic_add(size, &udp_prod_queue->rmem_alloc);

	if (!llist_add(&skb->ll_node, &udp_prod_queue->ll_root))
		return 0;

	dropcount = sock_flag(sk, SOCK_RXQ_OVFL) ? sk_drops_read(sk) : 0;

	spin_lock(&list->lock);

	ll_list = llist_del_all(&udp_prod_queue->ll_root);

	ll_list = llist_reverse_order(ll_list);

	llist_for_each_entry_safe(skb, next, ll_list, ll_node) {
		size = udp_skb_truesize(skb);
		total_size += size;
		err = udp_rmem_schedule(sk, size);
		if (unlikely(err)) {
			/*  Free the skbs outside of locked section. */
			skb->next = to_drop;
			to_drop = skb;
			continue;
		}

		q_size += size;
		sk_forward_alloc_add(sk, -size);

		/* no need to setup a destructor, we will explicitly release the
		 * forward allocated memory on dequeue
		 */
		SOCK_SKB_CB(skb)->dropcount = dropcount;
		nb++;
		__skb_queue_tail(list, skb);
	}

	atomic_add(q_size, &sk->sk_rmem_alloc);

	spin_unlock(&list->lock);

	if (!sock_flag(sk, SOCK_DEAD)) {
		/* Multiple threads might be blocked in recvmsg(),
		 * using prepare_to_wait_exclusive().
		 */
		while (nb) {
			INDIRECT_CALL_1(READ_ONCE(sk->sk_data_ready),
					sock_def_readable, sk);
			nb--;
		}
	}

	if (unlikely(to_drop)) {
		int err_ipv4 = 0;
		int err_ipv6 = 0;

		for (nb = 0; to_drop != NULL; nb++) {
			skb = to_drop;
			if (skb->protocol == htons(ETH_P_IP))
				err_ipv4++;
			else
				err_ipv6++;
			to_drop = skb->next;
			skb_mark_not_on_list(skb);
			sk_skb_reason_drop(sk, skb, SKB_DROP_REASON_PROTO_MEM);
		}
		numa_drop_add(&udp_sk(sk)->drop_counters, nb);
		if (err_ipv4 > 0) {
			SNMP_ADD_STATS(__UDPX_MIB(sk, true), UDP_MIB_MEMERRORS,
				       err_ipv4);
			SNMP_ADD_STATS(__UDPX_MIB(sk, true), UDP_MIB_INERRORS,
				       err_ipv4);
		}
		if (err_ipv6 > 0) {
			SNMP_ADD_STATS(__UDPX_MIB(sk, false), UDP_MIB_MEMERRORS,
				       err_ipv6);
			SNMP_ADD_STATS(__UDPX_MIB(sk, false), UDP_MIB_INERRORS,
				       err_ipv6);
		}
	}

	atomic_sub(total_size, &udp_prod_queue->rmem_alloc);

	return 0;

drop:
	udp_drops_inc(sk);
	return err;
}

void udp_destruct_common(struct sock *sk)
{
	/* reclaim completely the forward allocated memory */
	struct udp_sock *up = udp_sk(sk);
	unsigned int total = 0;
	struct sk_buff *skb;

	skb_queue_splice_tail_init(&sk->sk_receive_queue, &up->reader_queue);
	while ((skb = __skb_dequeue(&up->reader_queue)) != NULL) {
		total += skb->truesize;
		kfree_skb(skb);
	}
	udp_rmem_release(sk, total, 0, true);
	kfree(up->udp_prod_queue);
}

void skb_consume_udp(struct sock *sk, struct sk_buff *skb, int len)
{
	if (unlikely(READ_ONCE(udp_sk(sk)->peeking_with_offset)))
		sk_peek_offset_bwd(sk, len);

	if (!skb_shared(skb)) {
		skb_orphan(skb);
		skb_attempt_defer_free(skb);
		return;
	}

	if (!skb_unref(skb))
		return;

	/* In the more common cases we cleared the head states previously,
	 * see __udp_queue_rcv_skb().
	 */
	if (unlikely(udp_skb_has_head_state(skb)))
		skb_release_head_state(skb);
	__consume_stateless_skb(skb);
}

static struct sk_buff *__first_packet_length(struct sock *sk,
					     struct sk_buff_head *rcvq,
					     unsigned int *total)
{
	struct sk_buff *skb;

	while ((skb = skb_peek(rcvq)) != NULL) {
		if (udp_lib_checksum_complete(skb)) {
			struct net *net = sock_net(sk);

			__UDP_INC_STATS(net, UDP_MIB_CSUMERRORS);
			__UDP_INC_STATS(net, UDP_MIB_INERRORS);
			udp_drops_inc(sk);
			__skb_unlink(skb, rcvq);
			*total += skb->truesize;
			kfree_skb_reason(skb, SKB_DROP_REASON_UDP_CSUM);
		} else {
			udp_skb_csum_unnecessary_set(skb);
			break;
		}
	}
	return skb;
}

/**
 *	first_packet_length	- return length of first packet in receive queue
 *	@sk: socket
 *
 *	Drops all bad checksum frames, until a valid one is found.
 *	Returns the length of found skb, or -1 if none is found.
 */
static int first_packet_length(struct sock *sk)
{
	struct sk_buff_head *rcvq = &udp_sk(sk)->reader_queue;
	struct sk_buff_head *sk_queue = &sk->sk_receive_queue;
	unsigned int total = 0;
	struct sk_buff *skb;
	int res;

	spin_lock_bh(&rcvq->lock);
	skb = __first_packet_length(sk, rcvq, &total);
	if (!skb && !skb_queue_empty_lockless(sk_queue)) {
		spin_lock(&sk_queue->lock);
		skb_queue_splice_tail_init(sk_queue, rcvq);
		spin_unlock(&sk_queue->lock);

		skb = __first_packet_length(sk, rcvq, &total);
	}
	res = skb ? skb->len : -1;
	if (total)
		udp_rmem_release(sk, total, 1, false);
	spin_unlock_bh(&rcvq->lock);
	return res;
}

/*
 *	IOCTL requests applicable to the UDP protocol
 */

int udp_ioctl(struct sock *sk, int cmd, int *karg)
{
	switch (cmd) {
	case SIOCOUTQ:
	{
		*karg = sk_wmem_alloc_get(sk);
		return 0;
	}

	case SIOCINQ:
	{
		*karg = max_t(int, 0, first_packet_length(sk));
		return 0;
	}

	default:
		return -ENOIOCTLCMD;
	}

	return 0;
}

struct sk_buff *__skb_recv_udp(struct sock *sk, unsigned int flags,
			       int *off, int *err)
{
	struct sk_buff_head *sk_queue = &sk->sk_receive_queue;
	struct sk_buff_head *queue;
	struct sk_buff *last;
	long timeo;
	int error;

	queue = &udp_sk(sk)->reader_queue;
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	do {
		struct sk_buff *skb;

		error = sock_error(sk);
		if (error)
			break;

		error = -EAGAIN;
		do {
			spin_lock_bh(&queue->lock);
			skb = __skb_try_recv_from_queue(queue, flags, off, err,
							&last);
			if (skb) {
				if (!(flags & MSG_PEEK))
					udp_skb_destructor(sk, skb);
				spin_unlock_bh(&queue->lock);
				return skb;
			}

			if (skb_queue_empty_lockless(sk_queue)) {
				spin_unlock_bh(&queue->lock);
				goto busy_check;
			}

			/* refill the reader queue and walk it again
			 * keep both queues locked to avoid re-acquiring
			 * the sk_receive_queue lock if fwd memory scheduling
			 * is needed.
			 */
			spin_lock(&sk_queue->lock);
			skb_queue_splice_tail_init(sk_queue, queue);

			skb = __skb_try_recv_from_queue(queue, flags, off, err,
							&last);
			if (skb && !(flags & MSG_PEEK))
				udp_skb_dtor_locked(sk, skb);
			spin_unlock(&sk_queue->lock);
			spin_unlock_bh(&queue->lock);
			if (skb)
				return skb;

busy_check:
			if (!sk_can_busy_loop(sk))
				break;

			sk_busy_loop(sk, flags & MSG_DONTWAIT);
		} while (!skb_queue_empty_lockless(sk_queue));

		/* sk_queue is empty, reader_queue may contain peeked packets */
	} while (timeo &&
		 !__skb_wait_for_more_packets(sk, &sk->sk_receive_queue,
					      &error, &timeo,
					      (struct sk_buff *)sk_queue));

	*err = error;
	return NULL;
}
EXPORT_SYMBOL(__skb_recv_udp);

int udp_read_skb(struct sock *sk, skb_read_actor_t recv_actor)
{
	struct sk_buff *skb;
	int err;

try_again:
	skb = skb_recv_udp(sk, MSG_DONTWAIT, &err);
	if (!skb)
		return err;

	if (udp_lib_checksum_complete(skb)) {
		struct net *net = sock_net(sk);

		__UDP_INC_STATS(net, UDP_MIB_CSUMERRORS);
		__UDP_INC_STATS(net, UDP_MIB_INERRORS);
		udp_drops_inc(sk);
		kfree_skb_reason(skb, SKB_DROP_REASON_UDP_CSUM);
		goto try_again;
	}

	WARN_ON_ONCE(!skb_set_owner_sk_safe(skb, sk));

	/*
	 * skb->dev still aliases the UDP rx dev_scratch (its charge was freed
	 * on dequeue above); a sockmap verdict program may deref it via
	 * bpf_sk_lookup_*(), so clear it -> bpf_skc_lookup() uses skb->sk
	 */
	skb->dev = NULL;

	return recv_actor(sk, skb);
}

int udp_pre_connect(struct sock *sk, struct sockaddr_unsized *uaddr,
		    int addr_len)
{
	/* This check is replicated from __ip4_datagram_connect() and
	 * intended to prevent BPF program called below from accessing bytes
	 * that are out of the bound specified by user in addr_len.
	 */
	if (addr_len < sizeof(struct sockaddr_in))
		return -EINVAL;

	return BPF_CGROUP_RUN_PROG_INET4_CONNECT_LOCK(sk, uaddr, &addr_len);
}

int __udp_disconnect(struct sock *sk, int flags)
{
	struct inet_sock *inet = inet_sk(sk);
	/*
	 *	1003.1g - break association.
	 */

	sk->sk_state = TCP_CLOSE;
	inet->inet_daddr = 0;
	inet->inet_dport = 0;
	sock_rps_reset_rxhash(sk);
	sk->sk_bound_dev_if = 0;
	if (!(sk->sk_userlocks & SOCK_BINDADDR_LOCK)) {
		inet_reset_saddr(sk);
		if (sk->sk_prot->rehash &&
		    (sk->sk_userlocks & SOCK_BINDPORT_LOCK))
			sk->sk_prot->rehash(sk);
	}

	if (!(sk->sk_userlocks & SOCK_BINDPORT_LOCK)) {
		sk->sk_prot->unhash(sk);
		inet->inet_sport = 0;
	}
	sk_dst_reset(sk);
	return 0;
}
EXPORT_SYMBOL(__udp_disconnect);

int udp_disconnect(struct sock *sk, int flags)
{
	lock_sock(sk);
	__udp_disconnect(sk, flags);
	release_sock(sk);
	return 0;
}

void udp_lib_unhash(struct sock *sk)
{
	if (sk_hashed(sk)) {
		struct udp_hslot *hslot, *hslot2;
		struct net *net = sock_net(sk);
		struct udp_table *udptable;

		sock_rps_delete_flow(sk);
		udptable = net->ipv4.udp_table;
		hslot  = udp_hashslot(udptable, net, udp_sk(sk)->udp_port_hash);
		hslot2 = udp_hashslot2(udptable, udp_sk(sk)->udp_portaddr_hash);

		spin_lock_bh(&hslot->lock);
		if (rcu_access_pointer(sk->sk_reuseport_cb))
			reuseport_detach_sock(sk);
		if (sk_del_node_init_rcu(sk)) {
			hslot->count--;
			inet_sk(sk)->inet_num = 0;
			sock_prot_inuse_add(net, sk->sk_prot, -1);

			spin_lock(&hslot2->lock);
			hlist_del_init_rcu(&udp_sk(sk)->udp_portaddr_node);
			hslot2->count--;
			spin_unlock(&hslot2->lock);

			udp_unhash4(udptable, sk);
		}
		spin_unlock_bh(&hslot->lock);
	}
}

/*
 * inet_rcv_saddr was changed, we must rehash secondary hash
 */
void udp_lib_rehash(struct sock *sk, u16 newhash, u16 newhash4)
{
	if (sk_hashed(sk)) {
		struct udp_hslot *hslot, *hslot2, *nhslot2;
		struct net *net = sock_net(sk);
		struct udp_table *udptable;

		udptable = net->ipv4.udp_table;
		hslot = udp_hashslot(udptable, net, udp_sk(sk)->udp_port_hash);
		hslot2 = udp_hashslot2(udptable, udp_sk(sk)->udp_portaddr_hash);
		nhslot2 = udp_hashslot2(udptable, newhash);

		if (hslot2 != nhslot2 ||
		    rcu_access_pointer(sk->sk_reuseport_cb)) {
			/* we must lock primary chain too */
			spin_lock_bh(&hslot->lock);
			if (rcu_access_pointer(sk->sk_reuseport_cb))
				reuseport_detach_sock(sk);

			if (hslot2 != nhslot2) {
				spin_lock(&hslot2->lock);
				hlist_del_init_rcu(&udp_sk(sk)->udp_portaddr_node);
				hslot2->count--;
				spin_unlock(&hslot2->lock);

				spin_lock(&nhslot2->lock);
				hlist_add_head_rcu(&udp_sk(sk)->udp_portaddr_node,
							 &nhslot2->head);
				nhslot2->count++;
				spin_unlock(&nhslot2->lock);
			}

			spin_unlock_bh(&hslot->lock);
		}

		/* Now process hash4 if necessary:
		 * (1) update hslot4;
		 * (2) update hslot2->hash4_cnt.
		 * Note that hslot2/hslot4 should be checked separately, as
		 * either of them may change with the other unchanged.
		 */
		if (udp_hashed4(sk)) {
			spin_lock_bh(&hslot->lock);

			if (inet_rcv_saddr_any(sk)) {
				udp_unhash4(udptable, sk);
			} else {
				udp_rehash4(udptable, sk, newhash4);
				if (hslot2 != nhslot2) {
					spin_lock(&hslot2->lock);
					udp_hash4_dec(hslot2);
					spin_unlock(&hslot2->lock);

					spin_lock(&nhslot2->lock);
					udp_hash4_inc(nhslot2);
					spin_unlock(&nhslot2->lock);
				}
			}

			spin_unlock_bh(&hslot->lock);
		}

		udp_sk(sk)->udp_portaddr_hash = newhash;
	}
}

/* For TCP sockets, sk_rx_dst is protected by socket lock
 * For UDP, we use xchg() to guard against concurrent changes.
 */
bool udp_sk_rx_dst_set(struct sock *sk, struct dst_entry *dst)
{
	struct dst_entry *old;

	if (dst_hold_safe(dst)) {
		old = unrcu_pointer(xchg(&sk->sk_rx_dst, RCU_INITIALIZER(dst)));
		dst_release(old);
		return old != dst;
	}
	return false;
}

typedef struct sk_buff *(*udp_gro_receive_t)(struct sock *sk,
					     struct list_head *head,
					     struct sk_buff *skb);

static void set_xfrm_gro_udp_encap_rcv(__u16 encap_type, unsigned short family,
				       struct sock *sk)
{
#ifdef CONFIG_XFRM
	udp_gro_receive_t new_gro_receive;

	if (udp_test_bit(GRO_ENABLED, sk) && encap_type == UDP_ENCAP_ESPINUDP) {
		if (IS_ENABLED(CONFIG_IPV6) && family == AF_INET6)
			new_gro_receive = xfrm6_gro_udp_encap_rcv;
		else
			new_gro_receive = xfrm4_gro_udp_encap_rcv;

		if (udp_sk(sk)->gro_receive != new_gro_receive) {
			/*
			 * With IPV6_ADDRFORM the gro callback could change
			 * after being set, unregister the old one, if valid.
			 */
			if (udp_sk(sk)->gro_receive)
				udp_tunnel_update_gro_rcv(sk, false);

			WRITE_ONCE(udp_sk(sk)->gro_receive, new_gro_receive);
			udp_tunnel_update_gro_rcv(sk, true);
		}
	}
#endif
}

/*
 *	Socket option code for UDP
 */
int udp_lib_setsockopt(struct sock *sk, int level, int optname,
		       sockptr_t optval, unsigned int optlen,
		       int (*push_pending_frames)(struct sock *))
{
	struct udp_sock *up = udp_sk(sk);
	int val, valbool;
	int err = 0;

	if (level == SOL_SOCKET) {
		err = sk_setsockopt(sk, level, optname, optval, optlen);

		if (optname == SO_RCVBUF || optname == SO_RCVBUFFORCE) {
			sockopt_lock_sock(sk);
			/* paired with READ_ONCE in udp_rmem_release() */
			WRITE_ONCE(up->forward_threshold, sk->sk_rcvbuf >> 2);
			sockopt_release_sock(sk);
		}
		return err;
	}

	if (optlen < sizeof(int))
		return -EINVAL;

	if (copy_from_sockptr(&val, optval, sizeof(val)))
		return -EFAULT;

	valbool = val ? 1 : 0;

	switch (optname) {
	case UDP_CORK:
		if (val != 0) {
			udp_set_bit(CORK, sk);
		} else {
			udp_clear_bit(CORK, sk);
			lock_sock(sk);
			push_pending_frames(sk);
			release_sock(sk);
		}
		break;

	case UDP_ENCAP:
		sockopt_lock_sock(sk);
		switch (val) {
		case 0:
#ifdef CONFIG_XFRM
		case UDP_ENCAP_ESPINUDP:
			set_xfrm_gro_udp_encap_rcv(val, sk->sk_family, sk);
#if IS_ENABLED(CONFIG_IPV6)
			if (sk->sk_family == AF_INET6)
				WRITE_ONCE(up->encap_rcv,
					   xfrm6_udp_encap_rcv);
			else
#endif
				WRITE_ONCE(up->encap_rcv,
					   xfrm4_udp_encap_rcv);
#endif
			fallthrough;
		case UDP_ENCAP_L2TPINUDP:
			WRITE_ONCE(up->encap_type, val);
			udp_tunnel_encap_enable(sk);
			break;
		default:
			err = -ENOPROTOOPT;
			break;
		}
		sockopt_release_sock(sk);
		break;

	case UDP_NO_CHECK6_TX:
		udp_set_no_check6_tx(sk, valbool);
		break;

	case UDP_NO_CHECK6_RX:
		udp_set_no_check6_rx(sk, valbool);
		break;

	case UDP_SEGMENT:
		if (val < 0 || val > USHRT_MAX)
			return -EINVAL;
		WRITE_ONCE(up->gso_size, val);
		break;

	case UDP_GRO:
		sockopt_lock_sock(sk);
		/* when enabling GRO, accept the related GSO packet type */
		if (valbool)
			udp_tunnel_encap_enable(sk);
		udp_assign_bit(GRO_ENABLED, sk, valbool);
		udp_assign_bit(ACCEPT_L4, sk, valbool);
		set_xfrm_gro_udp_encap_rcv(up->encap_type, sk->sk_family, sk);
		sockopt_release_sock(sk);
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	return err;
}

int udp_lib_getsockopt(struct sock *sk, int level, int optname,
		       sockopt_t *opt)
{
	struct udp_sock *up = udp_sk(sk);
	int val, len;

	len = opt->optlen;
	/* keep the check so direct sockopt_t callers stay covered. */
	if (len < 0)
		return -EINVAL;

	len = min_t(unsigned int, len, sizeof(int));

	switch (optname) {
	case UDP_CORK:
		val = udp_test_bit(CORK, sk);
		break;

	case UDP_ENCAP:
		val = READ_ONCE(up->encap_type);
		break;

	case UDP_NO_CHECK6_TX:
		val = udp_get_no_check6_tx(sk);
		break;

	case UDP_NO_CHECK6_RX:
		val = udp_get_no_check6_rx(sk);
		break;

	case UDP_SEGMENT:
		val = READ_ONCE(up->gso_size);
		break;

	case UDP_GRO:
		val = udp_test_bit(GRO_ENABLED, sk);
		break;

	default:
		return -ENOPROTOOPT;
	}

	opt->optlen = len;
	if (copy_to_iter(&val, len, &opt->iter_out) != len)
		return -EFAULT;
	return 0;
}

/**
 * 	udp_poll - wait for a UDP event.
 *	@file: - file struct
 *	@sock: - socket
 *	@wait: - poll table
 *
 *	This is same as datagram poll, except for the special case of
 *	blocking sockets. If application is using a blocking fd
 *	and a packet with checksum error is in the queue;
 *	then it could get return from select indicating data available
 *	but then block when reading it. Add special case code
 *	to work around these arguably broken applications.
 */
__poll_t udp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	__poll_t mask = datagram_poll(file, sock, wait);
	struct sock *sk = sock->sk;

	if (!skb_queue_empty_lockless(&udp_sk(sk)->reader_queue))
		mask |= EPOLLIN | EPOLLRDNORM;

	/* Check for false positives due to checksum errors */
	if ((mask & EPOLLRDNORM) && !(file->f_flags & O_NONBLOCK) &&
	    !(sk->sk_shutdown & RCV_SHUTDOWN) && first_packet_length(sk) == -1)
		mask &= ~(EPOLLIN | EPOLLRDNORM);

	/* psock ingress_msg queue should not contain any bad checksum frames */
	if (sk_is_readable(sk))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;

}

int udp_abort(struct sock *sk, int err)
{
	if (!has_current_bpf_ctx())
		lock_sock(sk);

	/* udp{v6}_destroy_sock() sets it under the sk lock, avoid racing
	 * with close()
	 */
	if (sock_flag(sk, SOCK_DEAD))
		goto out;

	sk->sk_err = err;
	sk_error_report(sk);
	__udp_disconnect(sk, 0);

out:
	if (!has_current_bpf_ctx())
		release_sock(sk);

	return 0;
}

/* ------------------------------------------------------------------------ */
#ifdef CONFIG_PROC_FS

static unsigned short seq_file_family(const struct seq_file *seq);
static bool seq_sk_match(struct seq_file *seq, const struct sock *sk)
{
	unsigned short family = seq_file_family(seq);

	/* AF_UNSPEC is used as a match all */
	return ((family == AF_UNSPEC || family == sk->sk_family) &&
		net_eq(sock_net(sk), seq_file_net(seq)));
}

#ifdef CONFIG_BPF_SYSCALL
static const struct seq_operations bpf_iter_udp_seq_ops;
#endif

static struct sock *udp_get_first(struct seq_file *seq, int start)
{
	struct udp_iter_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct udp_table *udptable;
	struct sock *sk;

	udptable = net->ipv4.udp_table;

	for (state->bucket = start; state->bucket <= udptable->mask;
	     ++state->bucket) {
		struct udp_hslot *hslot = &udptable->hash[state->bucket];

		if (hlist_empty(&hslot->head))
			continue;

		spin_lock_bh(&hslot->lock);
		sk_for_each(sk, &hslot->head) {
			if (seq_sk_match(seq, sk))
				goto found;
		}
		spin_unlock_bh(&hslot->lock);
	}
	sk = NULL;
found:
	return sk;
}

static struct sock *udp_get_next(struct seq_file *seq, struct sock *sk)
{
	struct udp_iter_state *state = seq->private;
	struct net *net = seq_file_net(seq);
	struct udp_table *udptable;

	do {
		sk = sk_next(sk);
	} while (sk && !seq_sk_match(seq, sk));

	if (!sk) {
		udptable = net->ipv4.udp_table;

		if (state->bucket <= udptable->mask)
			spin_unlock_bh(&udptable->hash[state->bucket].lock);

		return udp_get_first(seq, state->bucket + 1);
	}
	return sk;
}

static struct sock *udp_get_idx(struct seq_file *seq, loff_t pos)
{
	struct sock *sk = udp_get_first(seq, 0);

	if (sk)
		while (pos && (sk = udp_get_next(seq, sk)) != NULL)
			--pos;
	return pos ? NULL : sk;
}

void *udp_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct udp_iter_state *state = seq->private;
	state->bucket = MAX_UDP_PORTS;

	return *pos ? udp_get_idx(seq, *pos-1) : SEQ_START_TOKEN;
}

void *udp_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct sock *sk;

	if (v == SEQ_START_TOKEN)
		sk = udp_get_idx(seq, 0);
	else
		sk = udp_get_next(seq, v);

	++*pos;
	return sk;
}

void udp_seq_stop(struct seq_file *seq, void *v)
{
	struct udp_iter_state *state = seq->private;
	struct udp_table *udptable;

	udptable = seq_file_net(seq)->ipv4.udp_table;

	if (state->bucket <= udptable->mask)
		spin_unlock_bh(&udptable->hash[state->bucket].lock);
}

/* ------------------------------------------------------------------------ */
static void udp4_format_sock(struct sock *sp, struct seq_file *f,
		int bucket)
{
	struct inet_sock *inet = inet_sk(sp);
	__be32 dest = inet->inet_daddr;
	__be32 src  = inet->inet_rcv_saddr;
	__u16 destp	  = ntohs(inet->inet_dport);
	__u16 srcp	  = ntohs(inet->inet_sport);

	seq_printf(f, "%5d: %08X:%04X %08X:%04X"
		" %02X %08X:%08X %02X:%08lX %08X %5u %8d %llu %d %pK %u",
		bucket, src, srcp, dest, destp, sp->sk_state,
		sk_wmem_alloc_get(sp),
		udp_rqueue_get(sp),
		0, 0L, 0,
		from_kuid_munged(seq_user_ns(f), sk_uid(sp)),
		0, sock_i_ino(sp),
		refcount_read(&sp->sk_refcnt), sp,
		sk_drops_read(sp));
}

static int udp4_seq_show(struct seq_file *seq, void *v)
{
	seq_setwidth(seq, 127);
	if (v == SEQ_START_TOKEN)
		seq_puts(seq, "   sl  local_address rem_address   st tx_queue "
			   "rx_queue tr tm->when retrnsmt   uid  timeout "
			   "inode ref pointer drops");
	else {
		struct udp_iter_state *state = seq->private;

		udp4_format_sock(v, seq, state->bucket);
	}
	seq_pad(seq, '\n');
	return 0;
}

#ifdef CONFIG_BPF_SYSCALL
struct bpf_iter__udp {
	__bpf_md_ptr(struct bpf_iter_meta *, meta);
	__bpf_md_ptr(struct udp_sock *, udp_sk);
	uid_t uid __aligned(8);
	int bucket __aligned(8);
};

union bpf_udp_iter_batch_item {
	struct sock *sk;
	__u64 cookie;
};

struct bpf_udp_iter_state {
	struct udp_iter_state state;
	unsigned int cur_sk;
	unsigned int end_sk;
	unsigned int max_sk;
	union bpf_udp_iter_batch_item *batch;
};

static int bpf_iter_udp_realloc_batch(struct bpf_udp_iter_state *iter,
				      unsigned int new_batch_sz, gfp_t flags);
static struct sock *bpf_iter_udp_resume(struct sock *first_sk,
					union bpf_udp_iter_batch_item *cookies,
					int n_cookies)
{
	struct sock *sk = NULL;
	int i;

	for (i = 0; i < n_cookies; i++) {
		sk = first_sk;
		udp_portaddr_for_each_entry_from(sk)
			if (cookies[i].cookie == atomic64_read(&sk->sk_cookie))
				goto done;
	}
done:
	return sk;
}

static struct sock *bpf_iter_udp_batch(struct seq_file *seq)
{
	struct bpf_udp_iter_state *iter = seq->private;
	struct udp_iter_state *state = &iter->state;
	unsigned int find_cookie, end_cookie;
	struct net *net = seq_file_net(seq);
	struct udp_table *udptable;
	unsigned int batch_sks = 0;
	int resume_bucket;
	int resizes = 0;
	struct sock *sk;
	int err = 0;

	resume_bucket = state->bucket;

	/* The current batch is done, so advance the bucket. */
	if (iter->cur_sk == iter->end_sk)
		state->bucket++;

	udptable = net->ipv4.udp_table;

again:
	/* New batch for the next bucket.
	 * Iterate over the hash table to find a bucket with sockets matching
	 * the iterator attributes, and return the first matching socket from
	 * the bucket. The remaining matched sockets from the bucket are batched
	 * before releasing the bucket lock. This allows BPF programs that are
	 * called in seq_show to acquire the bucket lock if needed.
	 */
	find_cookie = iter->cur_sk;
	end_cookie = iter->end_sk;
	iter->cur_sk = 0;
	iter->end_sk = 0;
	batch_sks = 0;

	for (; state->bucket <= udptable->mask; state->bucket++) {
		struct udp_hslot *hslot2 = &udptable->hash2[state->bucket].hslot;

		if (hlist_empty(&hslot2->head))
			goto next_bucket;

		spin_lock_bh(&hslot2->lock);
		sk = hlist_entry_safe(hslot2->head.first, struct sock,
				      __sk_common.skc_portaddr_node);
		/* Resume from the first (in iteration order) unseen socket from
		 * the last batch that still exists in resume_bucket. Most of
		 * the time this will just be where the last iteration left off
		 * in resume_bucket unless that socket disappeared between
		 * reads.
		 */
		if (state->bucket == resume_bucket)
			sk = bpf_iter_udp_resume(sk, &iter->batch[find_cookie],
						 end_cookie - find_cookie);
fill_batch:
		udp_portaddr_for_each_entry_from(sk) {
			if (seq_sk_match(seq, sk)) {
				if (iter->end_sk < iter->max_sk) {
					sock_hold(sk);
					iter->batch[iter->end_sk++].sk = sk;
				}
				batch_sks++;
			}
		}

		/* Allocate a larger batch and try again. */
		if (unlikely(resizes <= 1 && iter->end_sk &&
			     iter->end_sk != batch_sks)) {
			resizes++;

			/* First, try with GFP_USER to maximize the chances of
			 * grabbing more memory.
			 */
			if (resizes == 1) {
				spin_unlock_bh(&hslot2->lock);
				err = bpf_iter_udp_realloc_batch(iter,
								 batch_sks * 3 / 2,
								 GFP_USER);
				if (err)
					return ERR_PTR(err);
				/* Start over. */
				goto again;
			}

			/* Next, hold onto the lock, so the bucket doesn't
			 * change while we get the rest of the sockets.
			 */
			err = bpf_iter_udp_realloc_batch(iter, batch_sks,
							 GFP_NOWAIT);
			if (err) {
				spin_unlock_bh(&hslot2->lock);
				return ERR_PTR(err);
			}

			/* Pick up where we left off. */
			sk = iter->batch[iter->end_sk - 1].sk;
			sk = hlist_entry_safe(sk->__sk_common.skc_portaddr_node.next,
					      struct sock,
					      __sk_common.skc_portaddr_node);
			batch_sks = iter->end_sk;
			goto fill_batch;
		}

		spin_unlock_bh(&hslot2->lock);

		if (iter->end_sk)
			break;
next_bucket:
		resizes = 0;
	}

	WARN_ON_ONCE(iter->end_sk != batch_sks);
	return iter->end_sk ? iter->batch[0].sk : NULL;
}

static void *bpf_iter_udp_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct bpf_udp_iter_state *iter = seq->private;
	struct sock *sk;

	/* Whenever seq_next() is called, the iter->cur_sk is
	 * done with seq_show(), so unref the iter->cur_sk.
	 */
	if (iter->cur_sk < iter->end_sk)
		sock_put(iter->batch[iter->cur_sk++].sk);

	/* After updating iter->cur_sk, check if there are more sockets
	 * available in the current bucket batch.
	 */
	if (iter->cur_sk < iter->end_sk)
		sk = iter->batch[iter->cur_sk].sk;
	else
		/* Prepare a new batch. */
		sk = bpf_iter_udp_batch(seq);

	++*pos;
	return sk;
}

static void *bpf_iter_udp_seq_start(struct seq_file *seq, loff_t *pos)
{
	/* bpf iter does not support lseek, so it always
	 * continue from where it was stop()-ped.
	 */
	if (*pos)
		return bpf_iter_udp_batch(seq);

	return SEQ_START_TOKEN;
}

static int udp_prog_seq_show(struct bpf_prog *prog, struct bpf_iter_meta *meta,
			     struct udp_sock *udp_sk, uid_t uid, int bucket)
{
	struct bpf_iter__udp ctx;

	meta->seq_num--;  /* skip SEQ_START_TOKEN */
	ctx.meta = meta;
	ctx.udp_sk = udp_sk;
	ctx.uid = uid;
	ctx.bucket = bucket;
	return bpf_iter_run_prog(prog, &ctx);
}

static int bpf_iter_udp_seq_show(struct seq_file *seq, void *v)
{
	struct udp_iter_state *state = seq->private;
	struct bpf_iter_meta meta;
	struct bpf_prog *prog;
	struct sock *sk = v;
	uid_t uid;
	int ret;

	if (v == SEQ_START_TOKEN)
		return 0;

	lock_sock(sk);

	if (unlikely(sk_unhashed(sk))) {
		ret = SEQ_SKIP;
		goto unlock;
	}

	uid = from_kuid_munged(seq_user_ns(seq), sk_uid(sk));
	meta.seq = seq;
	prog = bpf_iter_get_info(&meta, false);
	ret = udp_prog_seq_show(prog, &meta, v, uid, state->bucket);

unlock:
	release_sock(sk);
	return ret;
}

static void bpf_iter_udp_put_batch(struct bpf_udp_iter_state *iter)
{
	union bpf_udp_iter_batch_item *item;
	unsigned int cur_sk = iter->cur_sk;
	__u64 cookie;

	/* Remember the cookies of the sockets we haven't seen yet, so we can
	 * pick up where we left off next time around.
	 */
	while (cur_sk < iter->end_sk) {
		item = &iter->batch[cur_sk++];
		cookie = sock_gen_cookie(item->sk);
		sock_put(item->sk);
		item->cookie = cookie;
	}
}

static void bpf_iter_udp_seq_stop(struct seq_file *seq, void *v)
{
	struct bpf_udp_iter_state *iter = seq->private;
	struct bpf_iter_meta meta;
	struct bpf_prog *prog;

	if (!v) {
		meta.seq = seq;
		prog = bpf_iter_get_info(&meta, true);
		if (prog)
			(void)udp_prog_seq_show(prog, &meta, v, 0, 0);
	}

	if (iter->cur_sk < iter->end_sk)
		bpf_iter_udp_put_batch(iter);
}

static const struct seq_operations bpf_iter_udp_seq_ops = {
	.start		= bpf_iter_udp_seq_start,
	.next		= bpf_iter_udp_seq_next,
	.stop		= bpf_iter_udp_seq_stop,
	.show		= bpf_iter_udp_seq_show,
};
#endif

static unsigned short seq_file_family(const struct seq_file *seq)
{
	const struct udp_seq_afinfo *afinfo;

#ifdef CONFIG_BPF_SYSCALL
	/* BPF iterator: bpf programs to filter sockets. */
	if (seq->op == &bpf_iter_udp_seq_ops)
		return AF_UNSPEC;
#endif

	/* Proc fs iterator */
	afinfo = pde_data(file_inode(seq->file));
	return afinfo->family;
}

static const struct seq_operations udp_seq_ops = {
	.start		= udp_seq_start,
	.next		= udp_seq_next,
	.stop		= udp_seq_stop,
	.show		= udp4_seq_show,
};

static struct udp_seq_afinfo udp4_seq_afinfo = {
	.family		= AF_INET,
};

static int __net_init udp4_proc_init_net(struct net *net)
{
	if (!proc_create_net_data("udp", 0444, net->proc_net, &udp_seq_ops,
			sizeof(struct udp_iter_state), &udp4_seq_afinfo))
		return -ENOMEM;
	return 0;
}

static void __net_exit udp4_proc_exit_net(struct net *net)
{
	remove_proc_entry("udp", net->proc_net);
}

static struct pernet_operations udp4_net_ops = {
	.init = udp4_proc_init_net,
	.exit = udp4_proc_exit_net,
};

int __init udp4_proc_init(void)
{
	return register_pernet_subsys(&udp4_net_ops);
}

void udp4_proc_exit(void)
{
	unregister_pernet_subsys(&udp4_net_ops);
}
#endif /* CONFIG_PROC_FS */

static __initdata unsigned long uhash_entries;
static int __init set_uhash_entries(char *str)
{
	ssize_t ret;

	if (!str)
		return 0;

	ret = kstrtoul(str, 0, &uhash_entries);
	if (ret)
		return 0;

	if (uhash_entries && uhash_entries < UDP_HTABLE_SIZE_MIN)
		uhash_entries = UDP_HTABLE_SIZE_MIN;
	return 1;
}
__setup("uhash_entries=", set_uhash_entries);

static void __init udp_table_init(struct udp_table *table, const char *name)
{
	unsigned int i, slot_size;

	slot_size = sizeof(struct udp_hslot) + sizeof(struct udp_hslot_main) +
		    udp_hash4_slot_size();
	table->hash = alloc_large_system_hash(name,
					      slot_size,
					      uhash_entries,
					      21, /* one slot per 2 MB */
					      0,
					      &table->log,
					      &table->mask,
					      UDP_HTABLE_SIZE_MIN,
					      UDP_HTABLE_SIZE_MAX);

	table->hash2 = (void *)(table->hash + (table->mask + 1));
	for (i = 0; i <= table->mask; i++) {
		INIT_HLIST_HEAD(&table->hash[i].head);
		table->hash[i].count = 0;
		spin_lock_init(&table->hash[i].lock);
	}
	for (i = 0; i <= table->mask; i++) {
		INIT_HLIST_HEAD(&table->hash2[i].hslot.head);
		table->hash2[i].hslot.count = 0;
		spin_lock_init(&table->hash2[i].hslot.lock);
	}
	udp_table_hash4_init(table);
}

u32 udp_flow_hashrnd(void)
{
	static u32 hashrnd __read_mostly;

	net_get_random_once(&hashrnd, sizeof(hashrnd));

	return hashrnd;
}
EXPORT_SYMBOL(udp_flow_hashrnd);

static void __net_init udp_sysctl_init(struct net *net)
{
	net->ipv4.sysctl_udp_rmem_min = PAGE_SIZE;
	net->ipv4.sysctl_udp_wmem_min = PAGE_SIZE;

#ifdef CONFIG_NET_L3_MASTER_DEV
	net->ipv4.sysctl_udp_l3mdev_accept = 0;
#endif
}

static struct udp_table __net_init *udp_pernet_table_alloc(unsigned int hash_entries)
{
	struct udp_table *udptable;
	unsigned int slot_size;
	int i;

	udptable = kmalloc_obj(*udptable);
	if (!udptable)
		goto out;

	slot_size = sizeof(struct udp_hslot) + sizeof(struct udp_hslot_main) +
		    udp_hash4_slot_size();
	udptable->hash = vmalloc_huge(hash_entries * slot_size,
				      GFP_KERNEL_ACCOUNT);
	if (!udptable->hash)
		goto free_table;

	udptable->hash2 = (void *)(udptable->hash + hash_entries);
	udptable->mask = hash_entries - 1;
	udptable->log = ilog2(hash_entries);

	for (i = 0; i < hash_entries; i++) {
		INIT_HLIST_HEAD(&udptable->hash[i].head);
		udptable->hash[i].count = 0;
		spin_lock_init(&udptable->hash[i].lock);

		INIT_HLIST_HEAD(&udptable->hash2[i].hslot.head);
		udptable->hash2[i].hslot.count = 0;
		spin_lock_init(&udptable->hash2[i].hslot.lock);
	}
	udp_table_hash4_init(udptable);

	return udptable;

free_table:
	kfree(udptable);
out:
	return NULL;
}

static void __net_exit udp_pernet_table_free(struct net *net)
{
	struct udp_table *udptable = net->ipv4.udp_table;

	if (udptable == &udp_table)
		return;

	kvfree(udptable->hash);
	kfree(udptable);
}

static void __net_init udp_set_table(struct net *net)
{
	struct udp_table *udptable;
	unsigned int hash_entries;
	struct net *old_net;

	if (net_eq(net, &init_net))
		goto fallback;

	old_net = current->nsproxy->net_ns;
	hash_entries = READ_ONCE(old_net->ipv4.sysctl_udp_child_hash_entries);
	if (!hash_entries)
		goto fallback;

	/* Set min to keep the bitmap on stack in udp_lib_get_port() */
	if (hash_entries < UDP_HTABLE_SIZE_MIN_PERNET)
		hash_entries = UDP_HTABLE_SIZE_MIN_PERNET;
	else
		hash_entries = roundup_pow_of_two(hash_entries);

	udptable = udp_pernet_table_alloc(hash_entries);
	if (udptable) {
		net->ipv4.udp_table = udptable;
	} else {
		pr_warn("Failed to allocate UDP hash table (entries: %u) "
			"for a netns, fallback to the global one\n",
			hash_entries);
fallback:
		net->ipv4.udp_table = &udp_table;
	}
}

static int __net_init udp_pernet_init(struct net *net)
{
#if IS_ENABLED(CONFIG_NET_UDP_TUNNEL)
	int i;

	/* No tunnel is configured */
	for (i = 0; i < ARRAY_SIZE(net->ipv4.udp_tunnel_gro); ++i) {
		INIT_HLIST_HEAD(&net->ipv4.udp_tunnel_gro[i].list);
		RCU_INIT_POINTER(net->ipv4.udp_tunnel_gro[i].sk, NULL);
	}
#endif
	udp_sysctl_init(net);
	udp_set_table(net);

	return 0;
}

static void __net_exit udp_pernet_exit(struct net *net)
{
	udp_pernet_table_free(net);
}

static struct pernet_operations __net_initdata udp_sysctl_ops = {
	.init	= udp_pernet_init,
	.exit	= udp_pernet_exit,
};

#if defined(CONFIG_BPF_SYSCALL) && defined(CONFIG_PROC_FS)
DEFINE_BPF_ITER_FUNC(udp, struct bpf_iter_meta *meta,
		     struct udp_sock *udp_sk, uid_t uid, int bucket)

static int bpf_iter_udp_realloc_batch(struct bpf_udp_iter_state *iter,
				      unsigned int new_batch_sz, gfp_t flags)
{
	union bpf_udp_iter_batch_item *new_batch;

	new_batch = kvmalloc_objs(*new_batch, new_batch_sz,
				  flags | __GFP_NOWARN);
	if (!new_batch)
		return -ENOMEM;

	if (flags != GFP_NOWAIT)
		bpf_iter_udp_put_batch(iter);

	memcpy(new_batch, iter->batch, sizeof(*iter->batch) * iter->end_sk);
	kvfree(iter->batch);
	iter->batch = new_batch;
	iter->max_sk = new_batch_sz;

	return 0;
}

#define INIT_BATCH_SZ 16

static int bpf_iter_init_udp(void *priv_data, struct bpf_iter_aux_info *aux)
{
	struct bpf_udp_iter_state *iter = priv_data;
	int ret;

	ret = bpf_iter_init_seq_net(priv_data, aux);
	if (ret)
		return ret;

	ret = bpf_iter_udp_realloc_batch(iter, INIT_BATCH_SZ, GFP_USER);
	if (ret)
		bpf_iter_fini_seq_net(priv_data);

	iter->state.bucket = -1;

	return ret;
}

static void bpf_iter_fini_udp(void *priv_data)
{
	struct bpf_udp_iter_state *iter = priv_data;

	bpf_iter_fini_seq_net(priv_data);
	kvfree(iter->batch);
}

static const struct bpf_iter_seq_info udp_seq_info = {
	.seq_ops		= &bpf_iter_udp_seq_ops,
	.init_seq_private	= bpf_iter_init_udp,
	.fini_seq_private	= bpf_iter_fini_udp,
	.seq_priv_size		= sizeof(struct bpf_udp_iter_state),
};

static struct bpf_iter_reg udp_reg_info = {
	.target			= "udp",
	.ctx_arg_info_size	= 1,
	.ctx_arg_info		= {
		{ offsetof(struct bpf_iter__udp, udp_sk),
		  PTR_TO_BTF_ID_OR_NULL | PTR_TRUSTED },
	},
	.seq_info		= &udp_seq_info,
};

static void __init bpf_iter_register(void)
{
	udp_reg_info.ctx_arg_info[0].btf_id = btf_sock_ids[BTF_SOCK_TYPE_UDP];
	if (bpf_iter_reg_target(&udp_reg_info))
		pr_warn("Warning: could not register bpf iterator udp\n");
}
#endif

void __init udp_init(void)
{
	unsigned long limit;

	udp_table_init(&udp_table, "UDP");
	limit = nr_free_buffer_pages() / 8;
	limit = max(limit, 128UL);
	sysctl_udp_mem[0] = limit / 4 * 3;
	sysctl_udp_mem[1] = limit;
	sysctl_udp_mem[2] = sysctl_udp_mem[0] * 2;

	if (register_pernet_subsys(&udp_sysctl_ops))
		panic("UDP: failed to init sysctl parameters.\n");

#if defined(CONFIG_BPF_SYSCALL) && defined(CONFIG_PROC_FS)
	bpf_iter_register();
#endif
}
