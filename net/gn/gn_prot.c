// SPDX-License-Identifier: GPL-2.0

/*
 * GeoNetworking
 */

#include <asm-generic/errno-base.h>
#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__
#include <linux/if_arp.h>
#include <linux/slab.h>
#include <linux/termios.h>
#include <net/sock.h>
#include <net/datalink.h>
#include <net/psnap.h>
#include <linux/gn.h>
#include <linux/gn_routing.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/etherdevice.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/if_ether.h>
#include <linux/bug.h>

struct datalink_proto *gn_dl;
static const struct proto_ops gn_dgram_ops;
static struct timer_list gn_beacon_timer;

/* Handlers for the socket list. */

DEFINE_RWLOCK(gn_sockets_lock);
HLIST_HEAD(gn_sockets);

static void __gn_insert_socket(struct sock *sk)
{
	sk_add_node(sk, &gn_sockets);
}

static void gn_remove_socket(struct sock *sk)
{
	write_lock_bh(&gn_sockets_lock);
	sk_del_node_init(sk);
	write_unlock_bh(&gn_sockets_lock);
}

#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*(var)), timer_fieldname)

static void gn_destroy_timer(struct timer_list *t)
{
	struct sock *sk = from_timer(sk, t, sk_timer);

	if (sk_has_allocations(sk)) {
		sk->sk_timer.expires = jiffies + SOCK_DESTROY_TIME;
		add_timer(&sk->sk_timer);
	} else {
		sock_put(sk);
	}
}

static void gn_destroy_socket(struct sock *sk)
{
	gn_remove_socket(sk);
	skb_queue_purge(&sk->sk_receive_queue);

	if (sk_has_allocations(sk)) {
		timer_setup(&sk->sk_timer, gn_destroy_timer, 0);
		sk->sk_timer.expires = jiffies + SOCK_DESTROY_TIME;
		add_timer(&sk->sk_timer);
	} else {
		sock_put(sk);
	}
}

/* Handling for system calls applied via the various interfaces to a
 * GeoNetworking socket object.
 */

static struct proto gn_proto = {
	.name = "GN",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct gn_sock),
};

HLIST_HEAD(gn_interfaces);
DEFINE_SPINLOCK(gn_interfaces_lock);

static void gn_activate_beacon(void);

/**
 * gn_iface - add device to the interface the socketaddress is bound to
 */
struct gn_iface *gn_if_add_device(struct net_device *dev,
				  struct sockaddr_gn *sa)
{
	struct gn_iface *new_gnif = kzalloc_obj(*new_gnif, GFP_KERNEL);
	struct gn_iface *gnif;
	bool was_empty;

	if (!new_gnif)
		return NULL;

	new_gnif->address = sa->sgn_addr;
	new_gnif->dev = dev;

	pr_info("Add interface %s with address %llx", dev->name,
		new_gnif->address);

	spin_lock_bh(&gn_interfaces_lock);
	hlist_for_each_entry_rcu(gnif, &gn_interfaces, hnode) {
		if (gnif->dev == dev) {
			// Replace existing interface address
			atomic_set(&new_gnif->local_sn,
				   atomic_read(&gnif->local_sn));
			memcpy(&new_gnif->pos, &gnif->pos, sizeof(gnif->pos));
			hlist_replace_rcu(&gnif->hnode, &new_gnif->hnode);
			spin_unlock_bh(&gn_interfaces_lock);
			kfree_rcu(gnif, rcu);
			return new_gnif;
		}
	}
	was_empty = hlist_empty(&gn_interfaces);

	// No existing interface address
	hlist_add_head_rcu(&new_gnif->hnode, &gn_interfaces);
	spin_unlock_bh(&gn_interfaces_lock);

	if (was_empty)
		gn_activate_beacon();

	return new_gnif;
}

void gn_if_drop_device(struct net_device *dev)
{
	struct hlist_node *tmp;
	struct gn_iface *gnif;

	spin_lock_bh(&gn_interfaces_lock);
	hlist_for_each_entry_safe(gnif, tmp, &gn_interfaces, hnode) {
		if (gnif->dev == dev) {
			hlist_del_rcu(&gnif->hnode);
			kfree_rcu(gnif, rcu);
			break;
		}
	}
	spin_unlock_bh(&gn_interfaces_lock);
}

static void gn_interfaces_clear(void)
{
	struct hlist_node *tmp;
	struct gn_iface *gnif;

	spin_lock_bh(&gn_interfaces_lock);
	hlist_for_each_entry_safe(gnif, tmp, &gn_interfaces, hnode) {
		hlist_del_rcu(&gnif->hnode);
		kfree_rcu(gnif, rcu);
	}
	spin_unlock_bh(&gn_interfaces_lock);
}

/*
 * find the interface to which the socketaddress is bound
 */
struct gn_iface *gn_find_interface(gn_address_t addr)
{
	struct gn_iface *gnif;
	bool found = false;

	rcu_read_lock();
	hlist_for_each_entry_rcu(gnif, &gn_interfaces, hnode) {
		if (gnif->address == addr) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found ? gnif : NULL;
}

/*
 * find an interface by the device it belongs to
 */
struct gn_iface *gn_find_interface_by_dev(struct net_device *dev)
{
	struct gn_iface *gnif;
	bool found = false;

	rcu_read_lock();
	hlist_for_each_entry_rcu(gnif, &gn_interfaces, hnode) {
		if (gnif->dev == dev) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found ? gnif : NULL;
}

/*
 * A device event has occurred. Watch for devices going down and
 * delete our use of them (iface and route).
 */
static int gn_device_event(struct notifier_block *this, unsigned long event,
			   void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	if (dev->type != ARPHRD_ETHER)
		return NOTIFY_DONE;

	if (event == NETDEV_DOWN || event == NETDEV_UNREGISTER)
		gn_if_drop_device(dev);

	return NOTIFY_DONE;
}

/*
 * Convert TAI timestamp in milliseconds to GN timestamp
 */
u32 gn_tai_to_gn(ktime_t tai_time)
{
	const ktime_t tai_offset = ms_to_ktime(GN_EPOCH_UNIX_MS);

	WARN_ONCE(ktime_before(tai_time, tai_offset),
		  "timestamp %lld out of bounds", tai_time);
	/* truncate timestamp (GN timestamp has 32 bits) */
	return (u32)ktime_sub(tai_time, tai_offset);
}

u32 gn_timestamp_now(void)
{
	return gn_tai_to_gn(ktime_get_clocktai());
}

/*
 * Create a socket. Initialise the socket, blank the addresses
 * set the state.
 */
static int gn_create(struct net *net, struct socket *sock, int protocol,
		     int kern)
{
	struct sock *sk;
	int rc;

	rc = -EAFNOSUPPORT;
	if (!net_eq(net, &init_net))
		goto out;

	rc = -ESOCKTNOSUPPORT;
	if (sock->type != SOCK_DGRAM)
		goto out;

	if (protocol < GN_PROTO_ANY || protocol > GN_PROTO_MAX)
		goto out;

	/* Note: Only BTP/GeoNetworking protocols supported; IPv6 encap not enabled */
	if (protocol == GN_PROTO_INET6)
		goto out;

	rc = -ENOMEM;
	sk = sk_alloc(net, PF_GN, GFP_KERNEL, &gn_proto, kern);
	if (!sk)
		goto out;
	rc = 0;
	sock->ops = &gn_dgram_ops;
	sock_init_data(sock, sk);
	if (protocol == GN_PROTO_ANY)
		gn_sk(sk)->protocol = GN_PROTO_BTP_A;
	else
		gn_sk(sk)->protocol = protocol;

	/* Checksums on by default */
//	sock_set_flag(sk, SOCK_ZAPPED);
out:
	return rc;
}

/* Free a socket. No work needed */
static int gn_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (sk) {
		sock_hold(sk);
		lock_sock(sk);

		sock_orphan(sk);
		sock->sk = NULL;
		gn_destroy_socket(sk);

		release_sock(sk);
		sock_put(sk);
	}
	return 0;
}

static int gn_pick_and_bind_port(struct sock *sk, struct sockaddr_gn *sgn)
{
	int retval;

	write_lock_bh(&gn_sockets_lock);

	for (sgn->sgn_port = GNPORT_RESERVED; sgn->sgn_port < GNPORT_LAST;
	     sgn->sgn_port++) {
		struct sock *s;

		sk_for_each(s, &gn_sockets) {
			struct gn_sock *gn = gn_sk(s);

			if (gn->src_port == sgn->sgn_port &&
			    gn->src_addr == sgn->sgn_addr)
				goto try_next_port;
		}

		/* Wheee, it's free, assign and insert. */
		__gn_insert_socket(sk);
		gn_sk(sk)->src_port = sgn->sgn_port;
		retval = 0;
		goto out;

try_next_port:;
	}

	retval = -EBUSY;
out:
	write_unlock_bh(&gn_sockets_lock);
	return retval;
}

static struct sock *gn_find_or_insert_socket(struct sock *sk,
					     struct sockaddr_gn *sgn)
{
	struct gn_sock *gn;
	struct sock *s;

	write_lock_bh(&gn_sockets_lock);
	sk_for_each(s, &gn_sockets) {
		gn = gn_sk(s);

		if (gn->src_port == sgn->sgn_port)
			goto found;
	}
	s = NULL;
	gn = gn_sk(sk);
	gn->src_addr = sgn->sgn_addr;
	gn->src_port = sgn->sgn_port;
	pr_info("gn: Add socket addr=%llx port=%d", gn->src_addr, gn->src_port);
	__gn_insert_socket(sk); /* Wheee, it's free, assign and insert. */
found:
	write_unlock_bh(&gn_sockets_lock);
	return s;
}

static int gn_autobind(struct sock *sock)
{
	return -EOPNOTSUPP;
}

/* Set the address 'our end' of the connection */
static int gn_bind(struct socket *sock, struct sockaddr_unsized *uaddr,
		   int addr_len)
{
	DECLARE_SOCKADDR(struct sockaddr_gn *, addr, uaddr);
	struct sock *sk = sock->sk;
	struct gn_sock *gn;
	int err;

	gn = gn_sk(sk);

	if (!sock_flag(sk, SOCK_ZAPPED) ||
	    addr_len != sizeof(struct sockaddr_gn))
		return -EINVAL;

	if (addr->sgn_family != AF_GN)
		return -EAFNOSUPPORT;

	lock_sock(sk);
	if (addr->sgn_addr != 0 && !gn_find_interface(addr->sgn_addr)) {
		release_sock(sk);
		return -EADDRNOTAVAIL;
	}
	gn->src_addr = addr->sgn_addr;

	if (addr->sgn_port == GNPORT_ANY) {
		err = gn_pick_and_bind_port(sk, addr);

		if (err < 0)
			goto out;
	} else {
		err = -EADDRINUSE;
		if (gn_find_or_insert_socket(sk, addr))
			goto out;
	}
	sock_reset_flag(sk, SOCK_ZAPPED);
	err = 0;

out:
	release_sock(sk);
	return err;
}

/* Set the address we talk to */
static int gn_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
		      int addr_len, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_gn *addr;
	struct gn_sock *gn;
	int err;

	gn = gn_sk(sk);

	sk->sk_state = TCP_CLOSE;
	sock->state = SS_UNCONNECTED;

	if (addr_len != sizeof(*addr))
		return -EINVAL;

	addr = (struct sockaddr_gn *)uaddr;

	if (addr->sgn_family != AF_GN)
		return -EAFNOSUPPORT;

	lock_sock(sk);
	err = -EBUSY;
	if (sock_flag(sk, SOCK_ZAPPED))
		if (gn_autobind(sk) < 0)
			goto out;

	/* Note: Route resolution for connected sockets occurs during gn_sendmsg */

	gn->dst_port = addr->sgn_port;
	gn->dst_addr = addr->sgn_addr;

	sock->state = SS_CONNECTED;
	sk->sk_state = TCP_ESTABLISHED;
	err = 0;
out:
	release_sock(sk);
	return err;
}

static struct sock *gn_search_socket(struct sockaddr_gn *tosgn,
				     struct gn_iface *gnif)
{
	struct gn_sock *gn;
	struct sock *s;

	read_lock_bh(&gn_sockets_lock);
	sk_for_each(s, &gn_sockets) {
		gn = gn_sk(s);

		if (gn->src_port != tosgn->sgn_port)
			continue;
		if (!gnif || gn->src_addr == gnif->address) {
			sock_hold(s);
			goto out;
		}
	}
	s = NULL;
out:
	read_unlock_bh(&gn_sockets_lock);
	return s;
}

static u16 gn_if_next_sn(struct gn_iface *gnif)
{
	return atomic_inc_return(&gnif->local_sn) % USHRT_MAX;
}

void gn_fill_sopv(struct gn_iface *gnif, struct gn_lpv *sopv, gn_address_t addr)
{
	/* Note: Speed and Heading zeroed until velocity sensors integrated */

	ktime_t tst = ktime_set(gnif->pos.tst.tv_sec, gnif->pos.tst.tv_nsec);
	// fill empty or invalid timestamp with current timestamp
	// (must be >= 2004-01-01 ETSI Epoch)
	if (tst == 0 || ktime_before(tst, ms_to_ktime(GN_EPOCH_UNIX_MS)))
		sopv->tst = cpu_to_be32(gn_timestamp_now());
	else
		sopv->tst = cpu_to_be32(gn_tai_to_gn(tst));
	sopv->lon = cpu_to_be32(gnif->pos.coord.lon);
	sopv->lat = cpu_to_be32(gnif->pos.coord.lat);
	sopv->addr = addr;
	sopv->spai = cpu_to_be16(FIELD_PREP(GN_LPV_PAI, 0) |
				 FIELD_PREP(GN_LPV_S, 0));
	sopv->h = cpu_to_be16(0);
}

static void gn_fill_bh_ch(struct gn_iface *gnif, struct gn_header *gh,
			  u8 packet_type, u8 packet_subtype, u8 rhl,
			  u8 next_header, __be16 payload_size)
{
	const u8 mhl = DEFAULT_HOP_LIMIT;
	/* rhl should never be greater than mhl */
	if (WARN_ON(rhl > mhl))
		rhl = DEFAULT_HOP_LIMIT;

	gh->gb_h.version = GN_VERSION;
	gh->gb_h.nh = BH_NH_COMMON_HEADER;
	gh->gb_h.reserved = 0;
	gh->gb_h.lt = BH_LT_DEFAULT;
	gh->gb_h.rhl = rhl;

	gh->gc_h.nh = next_header;
	gh->gc_h.reserved = 0;
	gh->gc_h.reserved2 = 0;
	gh->gc_h.ht = packet_type;
	gh->gc_h.hst = packet_subtype;
	gh->gc_h.tc = CH_TC_DEFAULT;
	gh->gc_h.pl = payload_size;
	if (packet_type == CH_HT_BEACON)
		gh->gc_h.mhl = BEACON_MHL;
	else
		gh->gc_h.mhl = DEFAULT_HOP_LIMIT;

	if (gnif->pos.flags & GN_POSITION_MOBILE)
		gh->gc_h.flags = CH_FLAG_MOBILE;
	else
		gh->gc_h.flags = 0;
}

static void gn_fill_bh_ch_nopayload(struct gn_iface *gnif,
				    struct gn_header *gn_h, u8 packet_type,
				    u8 packet_subtype, u8 rhl)
{
	gn_fill_bh_ch(gnif, gn_h, packet_type, packet_subtype, rhl, CH_NH_ANY,
		      0);
}

static void gn_location_service_req(struct gn_iface *gnif, gn_address_t saddr,
				    gn_address_t daddr)
{
	struct gn_ls_request_header *gls_h;
	struct gn_common_header *gc_h;
	struct gn_basic_header *gb_h;
	struct sk_buff *skb;
	int size = 0;

	size += gn_dl->header_length;
	size += gnif->dev->hard_header_len;
	size += sizeof(struct gn_basic_header);
	size += sizeof(struct gn_common_header);
	size += sizeof(struct gn_ls_request_header);

	skb = netdev_alloc_skb(gnif->dev, size);
	if (!skb)
		return;
	skb_reserve(skb, gn_dl->header_length);
	skb_reserve(skb, gnif->dev->hard_header_len);
	skb_reserve(skb, sizeof(struct gn_basic_header));
	skb_reserve(skb, sizeof(struct gn_common_header));
	skb_reserve(skb, sizeof(struct gn_ls_request_header));

	gls_h = skb_push(skb, sizeof(struct gn_ls_request_header));
	gc_h = skb_push(skb, sizeof(struct gn_common_header));
	gb_h = skb_push(skb, sizeof(struct gn_basic_header));

	gn_fill_bh_ch_nopayload(gnif, (struct gn_header *)gb_h, CH_HT_LS,
				CH_HST_LS_REQUEST, DEFAULT_HOP_LIMIT);

	gls_h->sn = cpu_to_be16(gn_if_next_sn(gnif));
	gls_h->reserved = 0;
	gls_h->addr = daddr;
	gn_fill_sopv(gnif, &gls_h->sopv, saddr);

	gn_dl->request(gn_dl, skb, gnif->dev->broadcast);
}

static void gn_location_service_reply(struct gn_spv *depv,
				      struct gn_iface *gnif, gn_address_t addr,
				      u64 llc)
{
	struct gn_ls_reply_header *gls_h;
	struct gn_common_header *gc_h;
	struct gn_basic_header *gb_h;
	struct sk_buff *skb;
	int size;

	size = 0;
	size += gn_dl->header_length;
	size += gnif->dev->hard_header_len;
	size += sizeof(struct gn_basic_header);
	size += sizeof(struct gn_common_header);
	size += sizeof(struct gn_ls_reply_header);

	skb = netdev_alloc_skb(gnif->dev, size);
	if (!skb)
		return;
	skb_reserve(skb, gn_dl->header_length);
	skb_reserve(skb, gnif->dev->hard_header_len);
	skb_reserve(skb, sizeof(struct gn_basic_header));
	skb_reserve(skb, sizeof(struct gn_common_header));
	skb_reserve(skb, sizeof(struct gn_ls_reply_header));

	gls_h = skb_push(skb, sizeof(struct gn_ls_reply_header));
	gc_h = skb_push(skb, sizeof(struct gn_common_header));
	gb_h = skb_push(skb, sizeof(struct gn_basic_header));

	gn_fill_bh_ch_nopayload(gnif, (struct gn_header *)gb_h, CH_HT_LS,
				CH_HST_LS_REPLY, DEFAULT_HOP_LIMIT);

	gls_h->sn = cpu_to_be16(gn_if_next_sn(gnif));
	gls_h->reserved = 0;
	gn_fill_sopv(gnif, &gls_h->sopv, addr);
	memcpy(&gls_h->depv, depv, sizeof(*depv));

	gn_dl->request(gn_dl, skb, gnif->dev->broadcast);
}

static int gn_pass_payload_sock(struct sockaddr_gn *tosgn, struct sk_buff *skb)
{
	int rc = NET_RX_DROP;
	struct sock *sock;

	sock = gn_search_socket(tosgn, NULL);
	if (!sock)
		return NET_RX_DROP;
	if (sock_queue_rcv_skb(sock, skb) == 0)
		rc = NET_RX_SUCCESS;
	sock_put(sock);
	return rc;
}

static int gn_forward_guc_packet(struct sk_buff *skb, gn_address_t dest_addr)
{
	struct sk_buff *forward_skb;
	u8 next_hop_mac[ETH_ALEN];
	struct gn_header *fwd_gh;
	struct gn_iface *gnif;

	fwd_gh = (struct gn_header *)skb_network_header(skb);
	if (fwd_gh->gb_h.rhl <= 0)
		return -EINVAL;

	gnif = gn_find_interface_by_dev(skb->dev);
	if (!gnif)
		return -ENODEV;

	forward_skb = skb_copy(skb, GFP_ATOMIC);
	if (!forward_skb) {
		pr_warn("Dropping packet during GUC forwarding\n");
		return -ENOMEM;
	}

	fwd_gh = (struct gn_header *)skb_network_header(forward_skb);
	fwd_gh->gb_h.rhl--;

	if (gn_query_ll_nexthop(gnif, dest_addr, next_hop_mac) == 0)
		gn_dl->request(gn_dl, forward_skb, next_hop_mac);
	else
		gn_dl->request(gn_dl, forward_skb, skb->dev->broadcast);

	return 0;
}

static int gn_forward_tsb_packet(struct sk_buff *skb)
{
	struct sk_buff *forward_skb;
	struct gn_header *fwd_gh;

	fwd_gh = (struct gn_header *)skb_network_header(skb);
	if (fwd_gh->gb_h.rhl <= 0)
		return -EINVAL;

	forward_skb = skb_copy(skb, GFP_ATOMIC);
	if (!forward_skb) {
		pr_warn("Dropping packet during TSB forwarding\n");
		return -ENOMEM;
	}

	fwd_gh = (struct gn_header *)skb_network_header(forward_skb);
	fwd_gh->gb_h.rhl--;

	gn_dl->request(gn_dl, forward_skb, skb->dev->broadcast);
	return 0;
}

static int gn_process_guc_packet(struct sk_buff *skb)
{
	struct btp_header *btp_h;
	struct sockaddr_gn tosgn;
	struct gn_iface *gnif;
	struct gn_header *gh;

	gh = (struct gn_header *)skb_network_header(skb);
	GN_SET_BTP(skb, btp_h, struct gn_guc_header);

	skb_reset_transport_header(skb);

	tosgn.sgn_family = PF_GN;
	tosgn.sgn_addr = gh->guc_h.depv.addr;
	tosgn.sgn_port = be16_to_cpu(btp_h->dst_port);

	if (gn_update_location_table(&gh->guc_h.sopv, false, NULL,
				     &gh->guc_h.sn))
		goto drop;

	gnif = gn_find_interface(tosgn.sgn_addr);
	if (!gnif) {
		if (gn_forward_guc_packet(skb, tosgn.sgn_addr) == 0) {
			kfree_skb(skb);
			return NET_RX_SUCCESS;
		}
		goto drop;
	}

	if (gn_pass_payload_sock(&tosgn, skb) != NET_RX_SUCCESS)
		goto drop;

	return NET_RX_SUCCESS;
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

static u8 gn_decode_shape(u8 hst)
{
	switch (hst) {
	case CH_HST_GABC_CIRCLE:
		return GN_SHAPE_CIRCLE;
	case CH_HST_GABC_RECT:
		return GN_SHAPE_RECTANGLE;
	case CH_HST_GABC_ELIP:
		return GN_SHAPE_ELLIPSE;
	default:
		return GN_SHAPE_UNSPECIFIED;
	};
}

static u8 gn_encode_shape(u8 shape)
{
	switch (shape) {
	case GN_SHAPE_CIRCLE:
		return CH_HST_GABC_CIRCLE;
	case GN_SHAPE_RECTANGLE:
		return CH_HST_GABC_RECT;
	case GN_SHAPE_ELLIPSE:
		return CH_HST_GABC_ELIP;
	default:
		return CH_HST_UNSPECIFIED;
	};
}

static struct gn_geo_scope gn_decode_geo_scope(struct gn_header *gh)
{
	struct gn_geo_scope scope = {
		.a = be16_to_cpu(gh->gbc_h.da),
		.b = be16_to_cpu(gh->gbc_h.db),
		.angle = be16_to_cpu(gh->gbc_h.angle),
		.coord.lat = be32_to_cpu(gh->gbc_h.gap_lat),
		.coord.lon = be32_to_cpu(gh->gbc_h.gap_lon),
		.shape = gn_decode_shape(gh->gc_h.hst),
	};
	return scope;
}

static int gn_process_gxc_packet(struct sk_buff *skb)
{
	unsigned long long next_hop_addr;
	struct gn_geo_scope scope;
	struct btp_header *btp_h;
	struct sockaddr_gn tosgn;
	struct gn_iface *gnif;
	struct gn_header *gh;
	bool run_dpd;
	s64 f_value;

	next_hop_addr = GN_BROADCAST_ADDR;

	gh = (struct gn_header *)skb_network_header(skb);
	GN_SET_BTP(skb, btp_h, struct gn_gxc_header);

	tosgn.sgn_family = PF_GN;
	tosgn.sgn_addr = gh->gbc_h.sopv.addr;
	tosgn.sgn_port = be16_to_cpu(btp_h->dst_port);

	/* Resolve local GeoNetworking interface to check area membership */
	gnif = gn_find_interface_by_dev(skb->dev);
	if (!gnif)
		goto drop;

	scope = gn_decode_geo_scope(gh);
	if (scope.shape == GN_SHAPE_UNSPECIFIED)
		goto drop;

	f_value = gn_F(gnif->pos.coord, scope);

	if (f_value >= 0) {
		// GeoAdhoc router is outside specified area
		switch (GN_AREA_FORWARDING) {
		case GN_AREA_FORWARDING_UNSPECIFIED:
		case GN_AREA_FORWARDING_SIMPLE:
			run_dpd = true;
			break;
		default:
			run_dpd = false;
			break;
		}
	} else {
		// GeoAdhoc router is inside specified area
		switch (GN_NON_AREA_FORWARDING) {
		case GN_NON_AREA_FORWARDING_UNSPECIFIED:
		case GN_NON_AREA_FORWARDING_GREEDY:
			run_dpd = true;
			break;
		default:
			run_dpd = false;
		}
	}
	if (gn_update_location_table(&gh->gbc_h.sopv, false, NULL,
				     run_dpd ? &gh->gbc_h.sn : NULL))
		goto drop;
	// Forwarding
	if (gh->gb_h.rhl > 0 && ((gh->gc_h.ht == CH_HT_GAC && f_value >= 0) ||
				 gh->gc_h.ht == CH_HT_GBC)) {
		struct gn_basic_header *fwd_gb_h;
		struct sk_buff *forward_skb;
		u8 dest_addr[ETH_ALEN];
		int rc;

		forward_skb = skb_copy(skb, GFP_ATOMIC);
		if (!forward_skb) {
			pr_debug("dropping packet");
			goto drop;
		}
		fwd_gb_h = (struct gn_basic_header *)skb_network_header(
			forward_skb);
		fwd_gb_h->rhl--;

		rc = gn_gxc_forward(gnif, f_value, dest_addr, &gh->gbc_h.sopv);
		switch (rc) {
		case GN_FORWARD_NEXT_HOP:
			gn_dl->request(gn_dl, forward_skb, dest_addr);
			break;
		case GN_FORWARD_BROADCAST:
			gn_dl->request(gn_dl, forward_skb, skb->dev->broadcast);
			break;
		case GN_FORWARD_BUFFER:
			pr_debug("forwarding buffers not implemented");
			kfree_skb(forward_skb);
			break;
		case GN_FORWARD_DISCARD:
			kfree_skb(forward_skb);
			break;
		default:
			pr_debug("internal error: unexpected return value");
			kfree_skb(forward_skb);
			goto drop;
		}
	}

	// Local delivery
	if (f_value >= 0) {
		if (gn_pass_payload_sock(&tosgn, skb) != NET_RX_SUCCESS)
			goto drop;
	} else {
		kfree_skb(skb);
	}

	return NET_RX_SUCCESS;
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

static int gn_process_shb_packet(struct sk_buff *skb, const u8 *ll_address)
{
	struct btp_header *btp_h;
	struct sockaddr_gn tosgn;
	struct gn_header *gh;

	gh = (struct gn_header *)skb_network_header(skb);
	GN_SET_BTP(skb, btp_h, struct gn_shb_header);

	/* Step 3: Duplicate Address Detection (DAD) check */

	// 4. update PV in the LocTE
	if (gn_update_location_table(&gh->shb_h.sopv, true, ll_address, NULL))
		goto drop;

	// 7. pass payload of GN_PDU to the upper protocol unit
	tosgn.sgn_family = PF_GN;
	tosgn.sgn_addr = gh->shb_h.sopv.addr;
	tosgn.sgn_port = be16_to_cpu(btp_h->dst_port);

	if (gn_pass_payload_sock(&tosgn, skb) != NET_RX_SUCCESS)
		goto drop;

	/* Step 8: Flush pending store-carry-forward buffers for source node */
	gn_ls_flush(gh->shb_h.sopv.addr);

	return NET_RX_SUCCESS;
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

static int gn_process_tsb_packet(struct sk_buff *skb)
{
	struct btp_header *btp_h;
	struct sockaddr_gn tosgn;
	struct gn_header *gh;

	gh = (struct gn_header *)skb_network_header(skb);
	GN_SET_BTP(skb, btp_h, struct gn_tsb_header);

	/* Step 3: Duplicate Address Detection (DAD) check */

	if (gn_update_location_table(&gh->tsb_h.sopv, false, NULL,
				     &gh->tsb_h.sn))
		goto drop;

	if (gh->gb_h.rhl > 0)
		gn_forward_tsb_packet(skb);

	// 7. pass payload of GN_PDU to the upper protocol unit
	tosgn.sgn_family = PF_GN;
	tosgn.sgn_addr = gh->tsb_h.sopv.addr;
	tosgn.sgn_port = be16_to_cpu(btp_h->dst_port);

	if (gn_pass_payload_sock(&tosgn, skb) != NET_RX_SUCCESS)
		goto drop;

	/* Step 8: Flush pending store-carry-forward buffers for source node */
	gn_ls_flush(gh->tsb_h.sopv.addr);

	return NET_RX_SUCCESS;
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

static int gn_process_beacon_packet(struct sk_buff *skb, const u8 *llc)
{
	struct gn_header *gh = (struct gn_header *)skb_network_header(skb);

	if (gn_update_location_table(&gh->beacon_h.sopv, true, llc, NULL)) {
		pr_debug("LocT update failure\n");
		kfree_skb(skb);
		return NET_RX_DROP;
	}

	gn_ls_flush(gh->beacon_h.sopv.addr);
	kfree_skb(skb);
	return NET_RX_SUCCESS;
}

static int gn_process_ls_packet(struct sk_buff *skb)
{
	gn_address_t dest_addr;
	struct gn_iface *gnif;
	struct gn_header *gh;

	gh = (struct gn_header *)skb_network_header(skb);

	if (gh->gc_h.hst == CH_HST_LS_REQUEST) {
		struct gn_ls_request_header *gls_req_h = &gh->ls_request_h;

		// 2. execute dad
		// 3. update loc_te
		dest_addr = gls_req_h->addr;
		if (gn_update_location_table(&gls_req_h->sopv, false, NULL,
					     &gls_req_h->sn))
			goto drop;
		// 4. find out if the packet has to be answered or forwarded
		gnif = gn_find_interface(dest_addr);
		if (gnif) {
			// has to be answered
			/* Note: gn_spv is a prefix (addr, tst, lat, lon) of gn_lpv */
			gn_location_service_reply(
				(struct gn_spv *)&gls_req_h->sopv, gnif,
				dest_addr, 0);
		} else {
			// has to be forwarded like a tsb
			// 5. try to flush own forward buffer
			gn_ls_flush(gls_req_h->sopv.addr);
			// 6. forward like a tsb
			gn_forward_tsb_packet(skb);
		}
	} else if (gh->gc_h.hst == CH_HST_LS_REPLY) {
		struct gn_ls_reply_header *gls_rep_h = &gh->ls_reply_h;
		// 2. execute dad
		// 3. update loc_te
		if (gn_update_location_table(&gls_rep_h->sopv, false, NULL,
					     &gls_rep_h->sn))
			goto drop;
		dest_addr = gls_rep_h->depv.addr;

		// 4. flush forward buffer
		gn_ls_flush(gls_rep_h->sopv.addr);
		// 5. find out if the packet has to be forwarded
		if (!gn_find_interface(dest_addr))
			gn_forward_guc_packet(skb, dest_addr);
	} else {
		goto drop;
	}
	kfree_skb(skb);
	return NET_RX_SUCCESS;
drop:
	//pr_info("Packet was dropped.");
	kfree_skb(skb);
	return NET_RX_DROP;
}

/**	gn_rcv() - Receive a packet (in skb) from device dev
 *	@skb - packet received
 *	@dev - network device where the packet comes from
 *	@pt - packet type
 *
 *	Receive a packet (in skb) from device dev. This has come from the SNAP
 *	decoder, and on entry skb->network_header is the GN Basic Header.
 *	The physical headers have been extracted.
 */
static int gn_rcv(struct sk_buff *skb, struct net_device *dev,
		  struct packet_type *pt, struct net_device *orig_dev)
{
	struct gn_header *gh;
	struct ethhdr *eth;

	if (!net_eq(dev_net(dev), &init_net))
		goto drop;

	if (dev->type != ARPHRD_ETHER)
		goto drop;

	if (!gn_find_interface_by_dev(dev))
		goto drop;

	skb = skb_share_check(skb, GFP_ATOMIC);

	if (!skb)
		goto drop;

	eth = (struct ethhdr *)skb_mac_header(skb);
	// TODO: validate
	if (skb->pkt_type == PACKET_LOOPBACK ||
	    ether_addr_equal(eth->h_source, dev->dev_addr))
		goto drop;

	skb_reset_network_header(skb);

	if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE))
		goto drop;

	// Basic Header processing
	gh = (struct gn_header *)skb_network_header(skb);

	if (gh->gb_h.version != GN_VERSION ||
	    gh->gb_h.nh != BH_NH_COMMON_HEADER) {
		pr_debug("corrupt packet");
		goto drop;
	}

	/*
	 * Retrieve information from socketbuffer an insert it into according headers;
	 * this is necessary to determine if the packet is ours and find the matching
	 * socket, or if the packet has to be forwarded
	 */
	// Common Header processing

	//skb_trim(skb, min_t(unsigned int, skb->len, gc_h->pl + sizeof(*gb_h)
	// + sizeof(*gc_h)));

	if (gh->gc_h.mhl < gh->gb_h.rhl)
		goto drop;

	switch (gh->gc_h.ht) {
	case CH_HT_GUC:
		if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE + GN_GUC_HLEN +
						BTP_HLEN))
			goto drop;
		return gn_process_guc_packet(skb);
	case CH_HT_GAC:
	case CH_HT_GBC:
		if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE +
						GN_GXC_HLEN + BTP_HLEN)
			goto drop;
		return gn_process_gxc_packet(skb);
	case CH_HT_TSB:
		if (gh->gc_h.hst == CH_HST_TSB_SINGLE_HOP) {
			if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE +
							GN_SHB_HLEN + BTP_HLEN))
				goto drop;
			return gn_process_shb_packet(skb, eth->h_source);
		}
		if (gh->gc_h.hst == CH_HST_TSB_MULTI_HOP) {
			if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE +
							GN_TSB_HLEN + BTP_HLEN))
				goto drop;
			return gn_process_tsb_packet(skb);
		}
		goto drop;
	case CH_HT_BEACON:
		if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE + GN_BEACON_HLEN))
			goto drop;
		return gn_process_beacon_packet(skb, eth->h_source);
	case CH_HT_LS:
		if (gh->gc_h.hst == CH_HST_LS_REQUEST) {
			if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE +
							GN_LS_REQUEST_HLEN))
				goto drop;
		} else if (gh->gc_h.hst == CH_HST_LS_REPLY) {
			if (!pskb_may_pull(skb, GN_BASE_HEADER_SIZE +
							GN_LS_REPLY_HLEN))
				goto drop;
		} else {
			goto drop;
		}
		return gn_process_ls_packet(skb);
	default:
		goto drop;
	}
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
}

static int gn_fill_guc_header(struct gn_guc_header *guc_h,
			      struct gn_iface *gnif, gn_address_t dest_addr,
			      struct gn_spv *depv, struct gn_sock *gn)
{
	gn_address_t saddr = gn->src_addr ? gn->src_addr : gnif->address;

	memset(guc_h, 0, sizeof(*guc_h));
	guc_h->sn = cpu_to_be16(gn_if_next_sn(gnif));
	gn_fill_sopv(gnif, &guc_h->sopv, saddr);
	guc_h->depv.addr = dest_addr;
	if (depv && depv->tst) {
		guc_h->depv.tst = depv->tst;
		guc_h->depv.lat = depv->lat;
		guc_h->depv.lon = depv->lon;
	} else {
		guc_h->depv.tst = htonl(0);
		guc_h->depv.lat = htonl(0);
		guc_h->depv.lon = htonl(0);
	}
	return 0;
}

static int gn_fill_gxc_header(struct gn_gxc_header *gxc_h,
			      struct gn_iface *gnif, struct gn_sock *gn)
{
	gn_address_t saddr = gn->src_addr ? gn->src_addr : gnif->address;

	WARN_ON_ONCE(gn->scope.scope_type != GN_SCOPE_GEOGRAPHICAL &&
		     gn->scope.scope_type != GN_SCOPE_GEOGRAPHICAL_ANYCAST);

	memset(gxc_h, 0, sizeof(*gxc_h));
	gxc_h->sn = cpu_to_be16(gn_if_next_sn(gnif));
	gn_fill_sopv(gnif, &gxc_h->sopv, saddr);

	gxc_h->gap_lat = cpu_to_be32(gn->scope.geo_scope.coord.lat);
	gxc_h->gap_lon = cpu_to_be32(gn->scope.geo_scope.coord.lon);
	gxc_h->angle = cpu_to_be16(gn->scope.geo_scope.angle);
	switch (gn->scope.geo_scope.shape) {
	case GN_SHAPE_CIRCLE:
		gxc_h->da = cpu_to_be16(gn->scope.geo_scope.a);
		break;
	case GN_SHAPE_ELLIPSE:
	case GN_SHAPE_RECTANGLE:
		gxc_h->da = cpu_to_be16(gn->scope.geo_scope.a);
		gxc_h->db = cpu_to_be16(gn->scope.geo_scope.b);
		break;
	default:
	case GN_SHAPE_UNSPECIFIED:
		pr_debug("unknown shape type");
		break;
	}
	return 0;
}

static int gn_fill_shb_header(struct gn_shb_header *shb_h,
			      struct gn_iface *gnif, struct gn_sock *gn)
{
	gn_address_t saddr = gn->src_addr ? gn->src_addr : gnif->address;

	memset(shb_h, 0, sizeof(*shb_h));
	gn_fill_sopv(gnif, &shb_h->sopv, saddr);

	// prefill media dependent data field with empty
	shb_h->mdd = htonl(0);
	return 0;
}

static int gn_fill_tsb_header(struct gn_tsb_header *tsb_h,
			      struct gn_iface *gnif, struct gn_sock *gn)
{
	gn_address_t saddr = gn->src_addr ? gn->src_addr : gnif->address;

	memset(tsb_h, 0, sizeof(*tsb_h));
	tsb_h->sn = cpu_to_be16(gn_if_next_sn(gnif));
	gn_fill_sopv(gnif, &tsb_h->sopv, saddr);
	return 0;
}

/**
 * gn_sendmsg - always called if sendmsg() is called on a socket with an
 * address family matching AF_GN
 * @msghdr - contains information, including the payload of the packet
 * @len - the size of the payload
 */
static int gn_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	DECLARE_SOCKADDR(struct sockaddr_gn *, usgn, msg->msg_name);
	int packet_type, packet_subtype, btp_type;
	struct gn_common_header *gc_h;
	struct gn_basic_header *gb_h;
	struct sockaddr_gn local_sgn;
	struct gn_spv depv = { 0 };
	u8 rhl = DEFAULT_HOP_LIMIT;
	int flags = msg->msg_flags;
	struct btp_header *btp_h;
	struct net_device *dev;
	struct gn_iface *gnif;
	struct sk_buff *skb;
	struct gn_sock *gn;
	int err = -EINVAL;
	struct sock *sk;
	u32 eh_size;
	void *gp_h;
	u32 size;

	sk = sock->sk;
	gn = gn_sk(sk);

	if (flags & ~(MSG_DONTWAIT | MSG_CMSG_COMPAT)) {
		pr_debug("unsupported sendmsg flags");
		return -EINVAL;
	}

	if (len > GN_MAXSZ) {
		pr_warn("message too long (got %zu, maximum %d)", len,
			GN_MAXSZ);
		return -EMSGSIZE;
	}
	lock_sock(sk);
	if (usgn) {
		err = -EBUSY;
		if (sock_flag(sk, SOCK_ZAPPED)) {
			pr_debug("socket zapped and autobind not implemented");
			if (gn_autobind(sk) < 0)
				goto out;
		}
		err = -EINVAL;
		if (msg->msg_namelen < sizeof(*usgn) ||
		    usgn->sgn_family != AF_GN) {
			pr_debug("incorrect address family\n");
			goto out;
		}
		//appletalk makes another check here
	} else {
		err = -ENOTCONN;
		if (sk->sk_state != TCP_ESTABLISHED)
			goto out;
		usgn = &local_sgn;
		usgn->sgn_family = AF_GN;
		usgn->sgn_port = gn->dst_port;
		usgn->sgn_addr = gn->dst_addr;
	}
	/*
	 * Before the packet can be send, we have determine if the gn_address of
	 * the source is bound to an interface. The interface provides the device
	 * for sending the packet.
	 */
	gnif = gn_find_interface(gn->src_addr);

	if (!gnif) {
		pr_debug("Could not find an interface\n");
		err = -EFAULT;
		goto out;
	}
	dev = gnif->dev;

	packet_subtype = CH_HST_UNSPECIFIED;
	if (usgn->sgn_addr != 0 && usgn->sgn_addr != GNADDR_BROADCAST &&
	    gn->scope.scope_type == GN_SCOPE_UNSPECIFIED) {
		packet_type = CH_HT_GUC;
	} else {
		switch (gn->scope.scope_type) {
		case GN_SCOPE_GEOGRAPHICAL:
			packet_type = CH_HT_GBC;
			packet_subtype =
				gn_encode_shape(gn->scope.geo_scope.shape);
			break;
		case GN_SCOPE_GEOGRAPHICAL_ANYCAST:
			packet_type = CH_HT_GAC;
			packet_subtype =
				gn_encode_shape(gn->scope.geo_scope.shape);
			break;
		case GN_SCOPE_TOPOLOGICAL:
			packet_type = CH_HT_TSB;
			if (gn->scope.topo_hops <= 1)
				packet_subtype = CH_HST_TSB_SINGLE_HOP;
			else
				packet_subtype = CH_HST_TSB_MULTI_HOP;
			break;
		case GN_SCOPE_UNSPECIFIED:
			packet_type = CH_HT_TSB;
			packet_subtype = CH_HST_TSB_SINGLE_HOP;
			break;
		default:
			err = -EINVAL;
			goto out;
		}
	}

	switch (gn->protocol) {
	case GN_PROTO_BTP_A:
		btp_type = CH_NH_BTPA;
		break;
	case GN_PROTO_BTP_B:
		btp_type = CH_NH_BTPB;
		break;
	default:
		err = -EINVAL;
		goto out;
	}

	// Determine extended (header type specific) header size
	switch (packet_type) {
	case CH_HT_GUC:
		eh_size = sizeof(struct gn_guc_header);
		break;
	case CH_HT_GAC:
	case CH_HT_GBC:
		eh_size = sizeof(struct gn_gxc_header);
		break;
	case CH_HT_TSB:
		if (packet_subtype == CH_HST_TSB_SINGLE_HOP) {
			eh_size = sizeof(struct gn_shb_header);
		} else if (packet_subtype == CH_HST_TSB_MULTI_HOP) {
			eh_size = sizeof(struct gn_tsb_header);
		} else {
			pr_debug("internal error");
			err = -EINVAL;
			goto out;
		}
		break;
	default:
		pr_debug("internal error");
		err = -EINVAL;
		goto out;
	}

	/*
	 * Find out, how much memory has to be allocated for the packet and
	 * allocate it.
	 */
	size = gn_dl->header_length;
	size += dev->hard_header_len;
	size += sizeof(struct gn_basic_header);
	size += sizeof(struct gn_common_header);
	size += eh_size;
	size += sizeof(struct btp_header);
	size += len;

	release_sock(sk);
	skb = sock_alloc_send_skb(sk, size, (flags & MSG_DONTWAIT), &err);
	lock_sock(sk);
	if (!skb) {
		err = -ENOMEM;
		goto out;
	}
	/*
	 * Reserve memory in the socketbuffer for datalink, hardware, our headers
	 */
	skb_reserve(skb, gn_dl->header_length);
	skb_reserve(skb, dev->hard_header_len);
	skb_reserve(skb, sizeof(struct gn_basic_header));
	skb_reserve(skb, sizeof(struct gn_common_header));
	skb_reserve(skb, eh_size);
	skb_reserve(skb, sizeof(struct btp_header));

	skb->dev = dev;
	/*
	 * Copy userdata from socket into the payload of the packet
	 */
	err = memcpy_from_msg(skb_put(skb, len), msg, len);
	if (err) {
		pr_debug("could not extract from msg\n");
		kfree_skb(skb);
		err = -EFAULT;
		goto out;
	}

	/*
	 * Get positions of headers and fill them
	 */
	btp_h = skb_push(skb, sizeof(struct btp_header));
	gp_h = skb_push(skb, eh_size);
	gc_h = skb_push(skb, sizeof(struct gn_common_header));
	gb_h = skb_push(skb, sizeof(struct gn_basic_header));

	switch (btp_type) {
	case CH_NH_BTPA:
		btp_h->src_port = htons(gn->src_port);
		btp_h->dst_port = htons(usgn->sgn_port);
		break;
	case CH_NH_BTPB:
		btp_h->dst_port = htons(usgn->sgn_port);
		btp_h->src_port = 0;
		break;
	default:
		pr_debug("internal error");
		kfree_skb(skb);
		err = -EINVAL;
		goto out;
	}

	switch (packet_type) {
	case CH_HT_GUC:
		gn_fill_depv(&depv, usgn->sgn_addr);
		gn_fill_guc_header((struct gn_guc_header *)gp_h, gnif,
				   usgn->sgn_addr, &depv, gn);
		break;
	case CH_HT_GAC:
	case CH_HT_GBC:
		gn_fill_gxc_header((struct gn_gxc_header *)gp_h, gnif, gn);
		break;
	case CH_HT_TSB:
		if (packet_subtype == CH_HST_TSB_SINGLE_HOP) {
			rhl = 1;
			gn_fill_shb_header((struct gn_shb_header *)gp_h, gnif,
					   gn);
		} else if (packet_subtype == CH_HST_TSB_MULTI_HOP) {
			//at this point it is safe to assume that topological scope is used
			rhl = gn->scope.topo_hops ?: DEFAULT_HOP_LIMIT;
			gn_fill_tsb_header((struct gn_tsb_header *)gp_h, gnif,
					   gn);
		} else {
			pr_debug("internal error");
			kfree_skb(skb);
			err = -EINVAL;
			goto out;
		}
		break;
	}

	/* Note: BTP header type (A/B) is determined by socket protocol */
	gn_fill_bh_ch(gnif, (struct gn_header *)gb_h, packet_type,
		      packet_subtype, rhl, btp_type,
		      htons(len + sizeof(struct btp_header)));

	/* Note: DEPV is populated during location service queue processing */
	if (packet_type == CH_HT_GUC) {
		u8 ll_address[ETH_ALEN];
		int queue_rc = gn_ls_queue(usgn->sgn_addr, skb);

		switch (queue_rc) {
		case GN_QUEUE_LS_STALE:
			// We need to perform a LS request
			gn_location_service_req(gnif, gn->src_addr,
						usgn->sgn_addr);
			break;
		case GN_QUEUE_LS_PENDING:
			// LS request is pending, we're done
			break;
		case GN_QUEUE_DIRECT:
			/* Destination in LocTE; resolve MAC or greedy next-hop */
			if (gn_query_ll_nexthop(gnif, usgn->sgn_addr,
						ll_address))
				gn_dl->request(gn_dl, skb, dev->broadcast);
			else
				gn_dl->request(gn_dl, skb, ll_address);
			break;
		case GN_QUEUE_ERROR:
		default:
			kfree_skb(skb);
			err = -ENOMEM;
			goto out;
		}
	} else {
		// Not a unicast packet
		gn_dl->request(gn_dl, skb, dev->broadcast);
	}

	/* Destination position vector routing handled by location service queue */

	err = len;
out:
	release_sock(sk);
	return err;
}

/**
 * gn_recvmsg - called, when recv() is called on a socket with AF = AF_GN.
 * Copies the data of a received packet into msg.
 * @sock - socket, on which recv() was called
 * @msg - buffer used to retrieve packetdata
 * @size - size of msg
 * @flags - flags for receiving
 *
 */

static int gn_recvmsg(struct socket *sock, struct msghdr *msg, size_t size,
		      int flags)
{
	struct sock *sk = sock->sk;
	struct gn_header *gh;
	struct sk_buff *skb;
	u16 copied = 0;
	int err = 0;
	u32 offset;

	/* It is necessary to find the actual length of the payload, which,
	 * in case of an unsecured package, resides in the commonheader and
	 * still has to be calculated without the length of the btp-header
	 * (4byte).
	 * Furthermore, the datapointer probably has to be set to the
	 * beginning of the actual payload
	 */
	skb = skb_recv_datagram(sk, flags & MSG_DONTWAIT, &err);
	lock_sock(sk);
	if (!skb)
		goto out;

	/* Note: Socket validation checks performed during bind/connect */
	gh = (struct gn_header *)skb_network_header(skb);

	copied = be16_to_cpu(gh->gc_h.pl);
	offset = skb->transport_header - skb->network_header +
		 sizeof(struct btp_header);
	copied -= sizeof(struct btp_header);

	/* Handle truncated datagram reception when user buffer is smaller than
	 * payload */
	if (copied > size) {
		copied = size;
		msg->msg_flags |= MSG_TRUNC;
	}

	err = skb_copy_datagram_msg(skb, offset, msg, copied);

	skb_free_datagram(sk, skb);

out:
	release_sock(sk);
	return err ?: copied;
}

/*
 * Geonetworking timer callbacks
 */
static int gn_send_beacon(struct gn_iface *gnif)
{
	/* Note: Media dependent procedures (e.g. ITS-G5 DCC / DCC Access)
	 * are evaluated here */
	struct gn_beacon_header *gbe_h;
	struct gn_common_header *gc_h;
	struct gn_basic_header *gb_h;
	struct sk_buff *skb;
	unsigned int size;

	size = gn_dl->header_length;
	size += gnif->dev->hard_header_len;
	size += sizeof(struct gn_basic_header);
	size += sizeof(struct gn_common_header);
	size += sizeof(struct gn_beacon_header);

	skb = netdev_alloc_skb(gnif->dev, size);
	if (!skb)
		goto out;

	skb_reserve(skb, size);
	skb->dev = gnif->dev;

	gbe_h = skb_push(skb, sizeof(struct gn_beacon_header));
	gc_h = skb_push(skb, sizeof(struct gn_common_header));
	gb_h = skb_push(skb, sizeof(struct gn_basic_header));

	gn_fill_bh_ch_nopayload(gnif, (struct gn_header *)gb_h, CH_HT_BEACON,
				CH_HST_BEACON, 1);

	gn_fill_sopv(gnif, &gbe_h->sopv, gnif->address);

	gn_dl->request(gn_dl, skb, gnif->dev->broadcast);

out:
	return 0;
}

static void gn_send_beacons(struct timer_list *tl)
{
	struct gn_iface *gnif;
	bool empty = true;

	rcu_read_lock();
	hlist_for_each_entry_rcu(gnif, &gn_interfaces, hnode) {
		gn_send_beacon(gnif);
		empty = false;
	}
	rcu_read_unlock();

	if (!empty)
		mod_timer(tl, jiffies + msecs_to_jiffies(
						GN_BEACON_RETRANSMIT_TIME));
}

static void gn_activate_beacon(void)
{
	mod_timer(&gn_beacon_timer,
		  jiffies + msecs_to_jiffies(GN_BEACON_RETRANSMIT_TIME));
}

static int validate_geo_scope(struct gn_geo_scope *scope)
{
	if (scope->angle > 360)
		return -EINVAL;

	if (scope->a == 0)
		return -EINVAL;

	switch (scope->shape) {
	case GN_SHAPE_CIRCLE:
		return scope->b != 0 ? -EINVAL : 0;
	case GN_SHAPE_RECTANGLE:
	case GN_SHAPE_ELLIPSE:
		return 0;
	default:
	case GN_SHAPE_UNSPECIFIED:
		return -EINVAL;
	}
	return 0;
}

static int validate_scope(struct gn_scope *scope)
{
	if (scope->scope_type < GN_SCOPE_TOPOLOGICAL ||
	    scope->scope_type > GN_SCOPE_MAX)
		return -EINVAL;
	switch (scope->scope_type) {
	case GN_SCOPE_TOPOLOGICAL:
		return 0;
	case GN_SCOPE_GEOGRAPHICAL:
	case GN_SCOPE_GEOGRAPHICAL_ANYCAST:
		return validate_geo_scope(&scope->geo_scope);
	default:
		return -EINVAL;
	}
	return 0;
}

static int gn_setsockopt(struct socket *sock, int level, int optname,
			 sockptr_t optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	int rc = -ENOPROTOOPT;
	struct gn_scope opt;
	struct gn_sock *gn;

	gn = gn_sk(sk);

	if (level != SOL_GN || optname != GN_SCOPE)
		goto out;

	rc = -EINVAL;
	if (optlen < sizeof(struct gn_scope))
		goto out;

	rc = -EFAULT;
	if (copy_from_sockptr(&opt, optval, sizeof(struct gn_scope)))
		goto out;

	rc = validate_scope(&opt);
	if (rc < 0)
		goto out;

	lock_sock(sk);
	memcpy(&gn->scope, &opt, sizeof(struct gn_scope));
	release_sock(sk);

	rc = 0;
out:
	return rc;
}

static int gn_getsockopt(struct socket *sock, int level, int optname,
			 char __user *optval, int __user *optlen)
{
	int len, rc = -ENOPROTOOPT;
	struct sock *sk = sock->sk;
	struct gn_sock *gn;

	gn = gn_sk(sk);

	if (level != SOL_GN || optname != GN_SCOPE)
		goto out;

	rc = -EFAULT;
	if (get_user(len, optlen))
		goto out;

	rc = -EINVAL;
	if (len < 0)
		goto out;

	len = min_t(unsigned int, len, sizeof(struct gn_scope));

	rc = -EFAULT;
	if (put_user(len, optlen))
		goto out;

	lock_sock(sk);
	rc = copy_to_user(optval, &gn->scope, len) ? -EFAULT : 0;
	release_sock(sk);
out:
	return rc;
}

/*
 * validate a gn_position coming from userspace
 */
int gn_validate_pos(struct gn_position *pos)
{
	if (pos->tst.tv_sec < 0 || pos->tst.tv_nsec < 0 ||
	    pos->tst.tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;
	return 0;
}

static int gn_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct sock *sk = sock->sk;
	int rc = -ENOIOCTLCMD;

	switch (cmd) {
	/* Protocol layer */
	case TIOCOUTQ: {
		long amount = sk->sk_sndbuf - sk_wmem_alloc_get(sk);

		if (amount < 0)
			amount = 0;
		rc = put_user(amount, (int __user *)argp);
		break;
	}
	case TIOCINQ: {
		/*
		 * These two are safe on a single CPU system as only
		 * user tasks fiddle here
		 */
		struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);
		long amount = 0;

		if (skb)
			amount = skb->len - sizeof(struct gn_header);
		rc = put_user(amount, (int __user *)argp);
		break;
	}
	case SIOCGSTAMP_OLD:
	case SIOCGSTAMP_NEW:
	case SIOCGSTAMPNS_OLD:
	case SIOCGSTAMPNS_NEW:
		break;
	}

	return rc;
}

#ifdef CONFIG_COMPAT
static int gn_compat_ioctl(struct socket *sock, unsigned int cmd,
			   unsigned long arg)
{
	/* All GeoNetworking ioctl commands (TIOCOUTQ, TIOCINQ, SIOCGIFADDR, etc.)
	 * and struct gn_position (using struct __kernel_timespec) are 64-bit
	 * clean and compat-safe.
	 */
	return gn_ioctl(sock, cmd, arg);
}
#endif

static const struct net_proto_family gn_family_ops = {
	.family = PF_GN,
	.create = gn_create,
	.owner = THIS_MODULE,
};

static const struct proto_ops gn_dgram_ops = {
	.family = PF_GN,
	.owner = THIS_MODULE,
	.release = gn_release,
	.bind = gn_bind,
	.connect = gn_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = sock_no_getname,
	.poll = datagram_poll,
	.ioctl = gn_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gn_compat_ioctl,
#endif
	.listen = sock_no_listen,
	.shutdown = sock_no_shutdown,
	.setsockopt = gn_setsockopt,
	.getsockopt = gn_getsockopt,
	.sendmsg = gn_sendmsg,
	.recvmsg = gn_recvmsg,
	.mmap = sock_no_mmap,
};

static struct notifier_block gn_notifier = {
	.notifier_call = gn_device_event,
};

static struct packet_type gn_packet_type __read_mostly = {
	.type = cpu_to_be16(ETH_P_GN),
	.func = gn_rcv,
};

/*
 * SNAP-ID for Geonetworking 0x8947
 * Note: SNAP header format uses big-endian 0x8947 per ETSI EN 302 636-4-1
 */
static unsigned char gn_snap_id[] = { 0x00, 0x00, 0x00, 0x89, 0x47 };

/* Called by proto.c on kernel start up */
static int __init gn_init(void)
{
	int rc;

	timer_setup(&gn_beacon_timer, gn_send_beacons, 0);

	rc = proto_register(&gn_proto, 0);
	if (rc)
		return rc;

	rc = sock_register(&gn_family_ops);
	if (rc)
		goto out_proto;

	gn_dl = register_snap_client(gn_snap_id, gn_rcv);
	if (!gn_dl) {
		pr_warn("Unable to register GeoNetworking with SNAP.\n");
		rc = -ENOMEM;
		goto out_snap;
	}

	dev_add_pack(&gn_packet_type);

	rc = register_netdevice_notifier(&gn_notifier);
	if (rc)
		goto out_dev;

#ifdef CONFIG_SYSCTL
	rc = gn_register_sysctl();
	if (rc)
		goto out_nd;
#endif

	rc = gn_netlink_init();
	if (rc)
		goto out_sysctl;

	return 0;

out_sysctl:
#ifdef CONFIG_SYSCTL
	gn_unregister_sysctl();
#endif
out_nd:
	unregister_netdevice_notifier(&gn_notifier);
out_dev:
	dev_remove_pack(&gn_packet_type);
	unregister_snap_client(gn_dl);
out_snap:
	sock_unregister(PF_GN);
out_proto:
	proto_unregister(&gn_proto);
	return rc;
}
module_init(gn_init);

static void __exit gn_exit(void)
{
	gn_netlink_exit();

#ifdef CONFIG_SYSCTL
	gn_unregister_sysctl();
#endif /* CONFIG_SYSCTL */

	timer_shutdown_sync(&gn_beacon_timer);

	unregister_netdevice_notifier(&gn_notifier);
	dev_remove_pack(&gn_packet_type);
	unregister_snap_client(gn_dl);
	sock_unregister(PF_GN);
	proto_unregister(&gn_proto);

	gn_interfaces_clear();
	gn_routing_exit();
	rcu_barrier();
}
module_exit(gn_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mr Noname <email.here@domain");
MODULE_DESCRIPTION("GeoNetworking protocol");
MODULE_ALIAS_NETPROTO(PF_GN);
