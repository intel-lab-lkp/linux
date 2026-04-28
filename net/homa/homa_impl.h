/* SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+ */

/* This file contains definitions that are shared across the files
 * that implement Homa for Linux.
 */

#ifndef _HOMA_IMPL_H
#define _HOMA_IMPL_H

#include <linux/bug.h>

#include <linux/audit.h>
#include <linux/icmp.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/proc_fs.h>
#include <linux/sched/signal.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/vmalloc.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/netns/generic.h>
#include <net/protocol.h>
#include <net/inet_common.h>
#include <net/gro.h>
#include <net/rps.h>

#include <linux/homa.h>
#include "homa_wire.h"

/* Forward declarations. */
struct homa;
struct homa_peer;
struct homa_rpc;
struct homa_sock;

/**
 * union sockaddr_in_union - Holds either an IPv4 or IPv6 address (smaller
 * and easier to use than sockaddr_storage).
 */
union sockaddr_in_union {
	/** @sa: Used to access as a generic sockaddr. */
	struct sockaddr sa;

	/** @in4: Used to access as IPv4 socket. */
	struct sockaddr_in in4;

	/** @in6: Used to access as IPv6 socket.  */
	struct sockaddr_in6 in6;
};

/**
 * struct homa - Stores overall information about the Homa transport, which
 * is shared across all Homa sockets and all network namespaces.
 */
struct homa {
	/**
	 * @next_outgoing_id: Id to use for next outgoing RPC request.
	 * This is always even: it's used only to generate client-side ids.
	 * Accessed without locks. Note: RPC ids are unique within a
	 * single client machine.
	 */
	atomic64_t next_outgoing_id;

	/**
	 * @peertab: Info about all the other hosts we have communicated with;
	 * includes peers from all network namespaces.
	 */
	struct homa_peertab *peertab;

	/**
	 * @socktab: Information about all open sockets. Dynamically
	 * allocated; must be kfreed.
	 */
	struct homa_socktab *socktab;

	/** @max_numa: Highest NUMA node id in use by any core. */
	int max_numa;

	/**
	 * @link_mbps: The raw bandwidth of the network uplink, in
	 * units of 1e06 bits per second.  Set externally via sysctl.
	 */
	int link_mbps;

	/**
	 * @resend_ticks: When an RPC's @silent_ticks reaches this value,
	 * start sending RESEND requests.
	 */
	int resend_ticks;

	/**
	 * @resend_interval: minimum number of homa timer ticks between
	 * RESENDs for the same RPC.
	 */
	int resend_interval;

	/**
	 * @timeout_ticks: abort an RPC if its silent_ticks reaches this value.
	 */
	int timeout_ticks;

	/**
	 * @timeout_resends: Assume that a server is dead if it has not
	 * responded after this many RESENDs have been sent to it.
	 */
	int timeout_resends;

	/**
	 * @request_ack_ticks: How many timer ticks we'll wait for the
	 * client to ack an RPC before explicitly requesting an ack.
	 * Set externally via sysctl.
	 */
	int request_ack_ticks;

	/**
	 * @reap_limit: Maximum number of packet buffers to free in a
	 * single call to home_rpc_reap.
	 */
	int reap_limit;

	/**
	 * @dead_buffs_limit: If the number of packet buffers in dead but
	 * not yet reaped RPCs is less than this number, then Homa reaps
	 * RPCs in a way that minimizes impact on performance but may permit
	 * dead RPCs to accumulate. If the number of dead packet buffers
	 * exceeds this value, then Homa switches to a more aggressive approach
	 * to reaping RPCs. Set externally via sysctl.
	 */
	int dead_buffs_limit;

	/**
	 * @max_dead_buffs: The largest aggregate number of packet buffers
	 * in dead (but not yet reaped) RPCs that has existed so far in a
	 * single socket.  Readable via sysctl, and may be reset via sysctl
	 * to begin recalculating.
	 */
	int max_dead_buffs;

	/**
	 * @max_gso_size: Maximum number of bytes that will be included
	 * in a single output packet that Homa passes to Linux. Can be set
	 * externally via sysctl to lower the limit already enforced by Linux.
	 */
	int max_gso_size;

	/**
	 * @gso_force_software: A non-zero value will cause Homa to perform
	 * segmentation in software using GSO; zero means ask the NIC to
	 * perform TSO. Set externally via sysctl.
	 */
	int gso_force_software;

	/**
	 * @wmem_max: Limit on the value of sk_sndbuf for any socket. Set
	 * externally via sysctl.
	 */
	int wmem_max;

	/**
	 * @timer_ticks: number of times that homa_timer has been invoked
	 * (may wraparound, which is safe).
	 */
	u32 timer_ticks;

	/**
	 * @flags: a collection of bits that can be set using sysctl
	 * to trigger various behaviors.
	 */
	int flags;

	/**
	 * @bpage_lease_usecs: how long a core can own a bpage (microseconds)
	 * before its ownership can be revoked to reclaim the page.
	 */
	int bpage_lease_usecs;

	/**
	 * @bpage_lease_cycles: same as bpage_lease_usecs except in
	 * homa_clock() units.
	 */
	int bpage_lease_cycles;

	/**
	 * @next_id: Set via sysctl; causes next_outgoing_id to be set to
	 * this value; always reads as zero. Typically used while debugging to
	 * ensure that different nodes use different ranges of ids.
	 */
	int next_id;

	/**
	 * @destroyed: True means that this structure is being destroyed
	 * so everyone should clean up.
	 */
	bool destroyed;

};

/**
 * struct homa_net - Contains Homa information that is specific to a
 * particular network namespace.
 */
struct homa_net {
	/** @homa: Global Homa information. */
	struct homa *homa;

	/**
	 * @prev_default_port: The most recent port number assigned from
	 * the range of default ports.
	 */
	u16 prev_default_port;

	/**
	 * @num_peers: The total number of struct homa_peers that exist
	 * for this namespace. Managed by homa_peer.c under the peertab lock.
	 */
	int num_peers;
};

/**
 * struct homa_skb_info - Additional information needed by Homa for each
 * outbound DATA packet. Space is allocated for this at the very end of the
 * linear part of the skb.
 */
struct homa_skb_info {
	/** @next_skb: used to link together outgoing skb's for a message. */
	struct sk_buff *next_skb;

	/**
	 * @wire_bytes: total number of bytes of network bandwidth that
	 * will be consumed by this packet. This includes everything,
	 * including additional headers added by GSO, IP header, Ethernet
	 * header, CRC, preamble, and inter-packet gap.
	 */
	int wire_bytes;

	/**
	 * @data_bytes: total bytes of message data across all of the
	 * segments in this packet.
	 */
	int data_bytes;

	/** @seg_length: maximum number of data bytes in each GSO segment. */
	int seg_length;

	/**
	 * @offset: offset within the message of the first byte of data in
	 * this packet.
	 */
	int offset;

	/** @rpc: RPC that this packet belongs to. */
	void *rpc;
};

/**
 * homa_get_skb_info() - Return the address of Homa's private information
 * for an sk_buff.
 * @skb:     Socket buffer whose info is needed.
 * Return: address of Homa's private information for @skb.
 */
static inline struct homa_skb_info *homa_get_skb_info(struct sk_buff *skb)
{
	return (struct homa_skb_info *)(skb_end_pointer(skb)) - 1;
}

/**
 * homa_set_doff() - Fills in the doff TCP header field for a packet.
 * @skb:   Packet whose doff field is to be set.
 * @size:  Size of the "header" in bytes (must be a multiple of 4). This is
 *         needed for two reasons. First, for TSO to work it must indicate
 *         the number of bytes that should be replicated in each segment.
 *         The bytes after this will be distributed among segments. Second,
 *         for TCP hijacking to work it must have a valid value (20 is a
 *         good choice if the packet isn't a TSO frame).
 */
static inline void homa_set_doff(struct sk_buff *skb, int size)
{
	tcp_hdr(skb)->doff = size >> 2;
}

/** skb_is_ipv6() - Return true if the packet is encapsulated with IPv6,
 *  false otherwise (presumably it's IPv4).
 */
static inline bool skb_is_ipv6(const struct sk_buff *skb)
{
	return ipv6_hdr(skb)->version == 6;
}

/**
 * ipv6_to_ipv4() - Given an IPv6 address produced by ipv4_to_ipv6, return
 * the original IPv4 address (in network byte order).
 * @ip6:  IPv6 address; assumed to be a mapped IPv4 address.
 * Return: IPv4 address stored in @ip6.
 */
static inline __be32 ipv6_to_ipv4(const struct in6_addr ip6)
{
	return ip6.in6_u.u6_addr32[3];
}

/**
 * canonical_ipv6_addr() - Convert a socket address to the "standard"
 * form used in Homa, which is always an IPv6 address; if the original address
 * was IPv4, convert it to an IPv4-mapped IPv6 address.
 * @addr:   Address to canonicalize (if NULL, "any" is returned).
 * Return: IPv6 address corresponding to @addr.
 */
static inline struct in6_addr canonical_ipv6_addr(const union sockaddr_in_union
						  *addr)
{
	struct in6_addr mapped;

	if (addr) {
		if (addr->sa.sa_family == AF_INET6)
			return addr->in6.sin6_addr;
		ipv6_addr_set_v4mapped(addr->in4.sin_addr.s_addr, &mapped);
		return mapped;
	}
	return in6addr_any;
}

/**
 * skb_canonical_ipv6_saddr() - Given a packet buffer, return its source
 * address in the "standard" form used in Homa, which is always an IPv6
 * address; if the original address was IPv4, convert it to an IPv4-mapped
 * IPv6 address.
 * @skb:   The source address will be extracted from this packet buffer.
 * Return: IPv6 address for @skb's source machine.
 */
static inline struct in6_addr skb_canonical_ipv6_saddr(struct sk_buff *skb)
{
	struct in6_addr mapped;

	if (skb_is_ipv6(skb))
		return ipv6_hdr(skb)->saddr;
	ipv6_addr_set_v4mapped(ip_hdr(skb)->saddr, &mapped);
	return mapped;
}

/**
 * homa_make_header_avl() - Invokes pskb_may_pull to make sure that all the
 * Homa header information for a packet is in the linear part of the skb
 * where it can be addressed using skb_transport_header.
 * @skb:     Packet for which header is needed.
 * Return:   The result of pskb_may_pull (true for success)
 */
static inline bool homa_make_header_avl(struct sk_buff *skb)
{
	int pull_length;

	pull_length = skb_transport_header(skb) - skb->data + HOMA_MAX_HEADER;
	if (pull_length > skb->len)
		pull_length = skb->len;
	return pskb_may_pull(skb, pull_length);
}

extern unsigned int homa_net_id;

void     homa_rpc_handoff(struct homa_rpc *rpc);
int      homa_xmit_control(enum homa_packet_type type, void *contents,
			   size_t length, struct homa_rpc *rpc);

int      homa_message_in_init(struct homa_rpc *rpc, int unsched);
void     homa_xmit_data(struct homa_rpc *rpc);

/**
 * homa_net() - Return the struct homa_net associated with a particular
 * struct net.
 * @net:     Get the Homa data for this net namespace.
 * Return:   see above.
 */
static inline struct homa_net *homa_net(struct net *net)
{
	return (struct homa_net *)net_generic(net, homa_net_id);
}

/**
 * homa_clock() - Return a fine-grain clock value that is monotonic and
 * consistent across cores.
 * Return: see above.
 */
static inline u64 homa_clock(void)
{
	/* This function exists to make it easy to switch time sources
	 * if/when new or better sources become available.
	 */
	return ktime_get_ns();
}

/**
 * homa_clock_khz() - Return the frequency of the values returned by
 * homa_clock, in units of KHz.
 * Return: see above.
 */
static inline u64 homa_clock_khz(void)
{
	return 1000000;
}

/**
 * homa_ns_to_cycles() - Convert from units of nanoseconds to units of
 * homa_clock().
 * @ns:      A time measurement in nanoseconds
 * Return:   The time in homa_clock() units corresponding to @ns.
 */
static inline u64 homa_ns_to_cycles(u64 ns)
{
	u64 tmp;

	tmp = ns * homa_clock_khz();
	do_div(tmp, 1000000);
	return tmp;
}

/* Homa Locking Strategy:
 *
 * (Note: this documentation is referenced in several other places in the
 * Homa code)
 *
 * In the Linux TCP/IP stack the primary locking mechanism is a sleep-lock
 * per socket. However, per-socket locks aren't adequate for Homa, because
 * sockets are "larger" in Homa. In TCP, a socket corresponds to a single
 * connection between two peers; an application can have hundreds or
 * thousands of sockets open at once, so per-socket locks leave lots of
 * opportunities for concurrency. With Homa, a single socket can be used for
 * communicating with any number of peers, so there will typically be just
 * one socket per thread. As a result, a single Homa socket must support many
 * concurrent RPCs efficiently, and a per-socket lock would create a bottleneck
 * (Homa tried this approach initially).
 *
 * Thus, the primary locks used in Homa spinlocks at RPC granularity. This
 * allows operations on different RPCs for the same socket to proceed
 * concurrently. Homa also has socket locks (which are spinlocks different
 * from the official socket sleep-locks) but these are used much less
 * frequently than RPC locks.
 *
 * Lock Ordering:
 *
 * There are several other locks in Homa besides RPC locks, all of which
 * are spinlocks. When multiple locks are held, they must be acquired in a
 * consistent order in order to prevent deadlock. Here are the rules for Homa:
 * 1. Except for RPC and socket locks, all locks should be considered
 *    "leaf" locks: don't acquire other locks while holding them.
 * 2. The lock order is:
 *    * RPC lock
 *    * Socket lock
 *    * Other lock
 *
 * It may seem surprising that RPC locks are acquired *before* socket locks,
 * but this is essential for high performance. Homa has been designed so that
 * many common operations (such as processing input packets) can be performed
 * while holding only an RPC lock; this allows operations on different RPCs
 * to proceed in parallel. Only a few operations, such as handing off an
 * incoming message to a waiting thread, require the socket lock. If socket
 * locks had to be acquired first, any operation that might eventually need
 * the socket lock would have to acquire it before the RPC lock, which would
 * severely restrict concurrency.
 *
 * Socket Shutdown:
 *
 * It is possible for socket shutdown to begin while operations are underway
 * that hold RPC locks but not the socket lock. For example, a new RPC
 * creation might be underway when a socket is shut down. The RPC creation
 * will eventually acquire the socket lock and add the new RPC to those
 * for the socket; it would be very bad if this were to happen after
 * homa_sock_shutdown things is has deleted all RPCs for the socket.
 * In general, any operation that acquires a socket lock must check
 * hsk->shutdown after acquiring the lock and abort if hsk->shutdown is set.
 *
 * Spinlock Implications:
 *
 * Homa uses spinlocks exclusively; this is needed because locks typically
 * need to be acquired at atomic level, such as in SoftIRQ code.
 *
 * Operations that can block, such as memory allocation and copying data
 * to/from user space, are not permitted while holding spinlocks (spinlocks
 * disable interrupts, so the holder must not block. This results in awkward
 * code in several places to move restricted operations outside locked
 * regions. Such code typically looks like this:
 *   - Acquire a reference on an object such as an RPC, in order to prevent
 *     the object from being deleted.
 *   - Release the object's lock.
 *   - Perform the restricted operation.
 *   - Re-acquire the lock.
 *   - Release the reference.
 * It is possible that the object may have been modified by some other party
 * while it was unlocked, so additional checks may be needed after reacquiring
 * the lock. As one example, an RPC may have been terminated, in which case
 * any operation in progress on that RPC should be aborted after reacquiring
 * the lock.
 *
 * Lists of RPCs:
 *
 * There are a few places where Homa needs to process all of the RPCs
 * associated with a socket, such as the timer. Such code must first lock
 * the socket (to protect access to the link pointers) then lock
 * individual RPCs on the list. However, this violates the rules for locking
 * order. It isn't safe to unlock the socket before locking the individual RPCs,
 * because RPCs could be deleted and their memory recycled between the unlock
 * of the socket lock and the lock of the RPC; this could result in corruption.
 * Homa uses two different approaches to handle this situation:
 * 1. Use ``homa_protect_rpcs`` to prevent RPC reaping for a socket. RPCs can
 *    still be terminated, but their memory won't go away until
 *    homa_unprotect_rpcs is invoked. This allows the socket lock to be
 *    released before acquiring RPC locks; after acquiring each RPC lock,
 *    the RPC must be checked to see if it has been terminated; if so, skip it.
 * 2. Use ``spin_trylock_bh`` to acquire the RPC lock while still holding the
 *    socket lock. If this fails, then release the socket lock and retry
 *    both the socket lock and the RPC lock. Of course, the state of both
 *    socket and RPC could change before the locks are finally acquired.
 */

#endif /* _HOMA_IMPL_H */
