// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/rhashtable.h>
#include <net/addrconf.h>
#include <net/arp.h>
#include <net/fib_notifier.h>
#include <net/ip6_fib.h>
#include <net/ipv6.h>
#include <net/ndisc.h>
#include <net/neighbour.h>
#include <net/netevent.h>
#include <net/nexthop.h>

#include "sparx5_main.h"
#include "sparx5_port.h"
#include "sparx5_vcap_ag_api.h"
#include "sparx5_vcap_impl.h"

/* The main routing objects are
 *
 * 1) Router legs, which correspond to IP interfaces. They can have multiple IPs
 *    attached and are associated with a VLAN. Only routes with nexthops that
 *    egress on ports that are part of a router leg are considered for
 *    offloading.
 *
 * 2) Fib entries, which correspond to entries in the routing table.
 *    > ip route add 6.6.0.0/16 nexthop via 1.0.10.10
 *    will create fib entry for 6.6.0.0/16, if it is offloadable.
 *
 * 3) Nexthop groups and nexthops. Each fib entry has exactly one nexthop hop.
 *    To simplify the SW representation, we duplicate nexthops and nexthops
 *    groups for each fib entry, even when the same nexthop is used for many
 *    different routes.
 *
 * 4) Neigh entries, which correspond to directly connected neighbours.
 *    These are created mainly by ARP events. Their job is to maintain the
 *    association of L3 and L2 addresses.
 *
 *    Neigh entries are anchored to the router leg they belong to, and live as
 *    long as that leg exists. They are also referenced by any nexthops using
 *    them as a gateway, so we keep track of those references.
 *
 *    If a neighbour which is used in a nexthop group dies, we will set the mac
 *    to zero, so traffic for this nexthop is trapped.
 *
 *
 * Both fib_entry and neigh_entry can trigger writes to the LPM VCAP, and own
 * entries in HW. Fib entries own the corresponding route associated with them.
 * Neigh entries own a /32 route, for traffic destined directly to the
 * neighbour.
 *
 * We have 3 main cases for routing:
 *
 * 1) Routes for directly connected subnets. E.g. router has IP 1.0.10.1, and
 *    routes subnet 1.0.10.0/24.
 *
 *    In this case, we have a fib_entry, with a non-gateway nexthop group. We
 *    install a LPM VCAP route, with action type arp entry, for 1.0.10.0/24 with
 *    a zero mac, to ensure frames destined for this subnet will be sent to the
 *    CPU and start the ARP process.
 *
 *    When we get the arp reply, we create a neigh_entry for each neighbour and
 *    install a direct route in the LPM VCAP for this neighbour. For example,
 *    1.0.10.11 is sent to DMAC 0x1111110001. This is how we route directly
 *    connected subnets.
 *
 *    Moreover, each router leg maintains a list of all neighbours discovered
 *    on it. Neighbours are anchored to the leg, not to the covering route, so
 *    they persist as long as the leg exists, independent of which fib entry
 *    covers their subnet.
 *
 *   +-------------+            |-> neigh_entry 1.0.10.10
 *   | router leg  | neigh_list |-> neigh_entry 1.0.10.11
 *   |   1.0.10.1  |------------|-> neigh_entry 1.0.10.12
 *   +-------------+
 *
 *   +-------------+    +--------------+
 *   |  fib_entry  |    | nexthop group|
 *   | 1.0.10.0/24 |----+              |
 *   +-------------+    |  nexthop --------> NULL (Write zero mac in VCAP)
 *                      +--------------+
 *
 *
 * 2) Routes for non-connected subnets. E.g. we are routing subnet 6.6.0.0/16,
 *    but we have no IP in this subnet. We are routing via nexthops, which are
 *    directly connected. Say we have >=2 nexthops.
 *
 *    In this case, have a fib_entry with a gateway nexthop group. Each nexthop
 *    points to a neigh_entry, corresponding to the gateway used for routing.
 *
 *    Say we use the nexthops:
 *
 *    - 1.0.11.10
 *    - 1.0.10.10
 *    - 1.0.9.10
 *
 *    We install a LPM VCAP route for 6.6.0.0/16, which contains a pointer
 *    to the hw arp table, and the size of the group. The ARP table
 *    contains the Mac addresses of the nexthops. The Mac addresses are
 *    supplied by neigh_entries.
 *
 *
 *   +-------------+    +--------------+
 *   |  fib_entry  |    | nexthop group|
 *   | 6.6.0.0/164 |----+              |
 *   +-------------+    |  nexthop --------> neigh_entry 1.0.11.10
 *                      |  nexthop --------> neigh_entry 1.0.10.10
 *                      |  nexthop --------> neigh_entry 1.0.9.10
 *                      +--------------+
 *
 *
 * 3) Local routes for traffic destined to the router. If the router has an IP
 *    1.0.10.1, then we must ensure this traffic is sent to the CPU.
 *    Therefore, we install a direct route for 1.0.10.1/32 in the LPM VCAP, with
 *    zero mac.
 *
 * 4) All IPv6 link-local traffic is explicitly trapped.
 *
 * On the hardware side, we use the VCAP LPM and ARP table for UC IPv4
 * routing. HW picks the match found at the highest address in the VCAP LPM.
 * To ensure the longest prefix match we make sure to order the entries
 * according to mask length, with longer masks at higher addresses.
 *
 * It is possible to store ARP data, such as DMAC, directly in the VCAP LPM
 * using ARP entry actions. We do this whenever possible, so the ARP table
 * is only used when a route has multiple nexthops.
 *
 * With the above breakdown in mind, cases 1) and 3) use arp entries, and
 * case 2) use the arp table if the number of nexthops is >1.
 *
 * If the DMAC written to HW is all zero, the chip will trap the frame,
 * redirecting it to the CPU. This is how we get the kernel to perform ARP
 * requests on our behalf.
 *
 * The nexthop group must be laid out at contiguous addresses in the ARP table.
 * The VCAP LPM stores a pointer to the bottom address in the group, and the
 * group size. We do not use the arp pointer remap table.
 *
 * The layout of nexthops in a nexthop group matches the layout in HW, e.g.
 *
 * nhgi->nexthops[0] -> arp table address n
 * ...
 * nhgi->nexthops[k] -> arp table address n+k
 *
 * where the n is the ARP table offset (atbl_offset) for the group.
 */

#define SPARX5_MAX_ECMP_SIZE		16
#define SPARX5_RLEG_USE_GLOBAL_BASE_MAC	2
#define SPARX5_LINK_LOCAL_PREFIX_LEN	64
#define SPARX5_BLACKHOLE_VMID(spx5)	((spx5)->data->consts->vmid_cnt - 1)
/* The rewriter field REW_RLEG_CTRL_RLEG_EVID will be written with the
 * blackhole vid, but it is only 12 bits wide. However, this is only used for
 * routing based rewrites on egress, which is not used for the blackhole.
 * The important field is ANA_L3_RLEG_CTRL_RLEG_EVID which is 13 bits wide.
 * Therefore, it is safe to use VIDs wider than 12 bits for the blackhole
 * vid, frames will not be forwarded into VLAN 0s port mask.
 */
#define SPARX5_BLACKHOLE_VID		VLAN_N_VID

struct sparx5_rr_fib6_entry_info {
	struct fib6_info **rt_arr;
	unsigned int nrt6;
};

enum sparx5_rr_l3_version {
	SPARX5_IPV4 = 0,
	SPARX5_IPV6,
};

#define SPARX5_IADDR_LEN(v)		((v) == SPARX5_IPV4 ? 32 : 128)
/* Order longer prefixes at high addresses. */
#define LPM_SORT_KEY(plen)		(SPARX5_IADDR_LEN(SPARX5_IPV6) - (plen))

struct sparx5_rr_fib_info {
	union {
		struct fib_entry_notifier_info fen4_info;
		struct sparx5_rr_fib6_entry_info fe6_info;
	};
	enum sparx5_rr_l3_version version;
};

struct sparx5_fib_event_work {
	struct work_struct work;
	struct sparx5_rr_fib_info fi;
	struct sparx5 *sparx5;
	unsigned long event;
};

struct sparx5_rr_netevent_work {
	struct work_struct work;
	struct sparx5 *sparx5;
	struct neighbour *neigh;
	unsigned long event;
};

struct sparx5_rr_router_leg {
	struct net_device *dev;
	netdevice_tracker dev_tracker;
	struct sparx5 *sparx5;
	struct list_head leg_list_node; /* Router member */
	struct list_head neigh_list; /* Neighbours bound to this leg */
	u16 vmid; /* Internal id */
	u32 vid; /* VLAN id */
};

struct sparx5_iaddr {
	union {
		__be32 ipv4;
		struct in6_addr ipv6;
	}; /* Must be first */
	enum sparx5_rr_l3_version version;
};

#define LPM_PROTO(iaddr) ((iaddr)->version ? ETH_P_IPV6 : ETH_P_IP)

struct sparx5_rr_neigh_entry {
	struct sparx5_rr_neigh_key {
		struct net_device *dev;
		struct sparx5_iaddr iaddr;
	} key;
	struct rhash_head ht_node;
	struct sparx5_rr_router_leg *leg;
	struct list_head leg_list_node; /* Member of leg->neigh_list */
	struct neigh_table *neigh_tbl; /* Kernel neighbour table */
	struct list_head nexthop_list; /* Nexthops using this neigh entry */
	struct sparx5_rr_hw_route hw_route;
	unsigned char hwaddr[ETH_ALEN];
	u16 vmid;
	bool connected;
};

struct sparx5_rr_nexthop {
	struct sparx5_rr_neigh_entry *neigh_entry;
	struct sparx5_rr_nexthop_group *grp;
	struct list_head neigh_list_node; /* Neigh entry member */
	struct neigh_table *neigh_tbl; /* Kernel neighbour table */
	struct sparx5_iaddr gw_addr;
	int ifindex;
	bool gateway;
	bool trapped;
};

struct sparx5_rr_nexthop_group_info {
	struct sparx5_rr_nexthop_group *grp;
	u16 atbl_offset;
	bool atbl_offset_valid;
	u8 count; /* HW allows up to 16 nexthops */
	struct sparx5_rr_nexthop nexthops[] __counted_by(count);
};

struct sparx5_rr_nexthop_group {
	struct sparx5_rr_fib_entry *fib_entry;
	struct sparx5_rr_nexthop_group_info *nhgi;
};

enum sparx5_rr_fib_type {
	SPARX5_RR_FIB_TYPE_INVALID = 0,
	SPARX5_RR_FIB_TYPE_LOCAL,
	SPARX5_RR_FIB_TYPE_UNICAST,
	SPARX5_RR_FIB_TYPE_MULTICAST,
	SPARX5_RR_FIB_TYPE_BLACKHOLE,
	SPARX5_RR_FIB_TYPE_PROHIBIT,
	SPARX5_RR_FIB_TYPE_UNREACHABLE,
};

struct sparx5_rr_fib_key {
	struct sparx5_iaddr addr;
	u32 prefix_len;
	u32 tb_id; /* Routing table type: RT_TABLE_* */
};

struct sparx5_rr_fib_entry {
	struct sparx5_rr_fib_key key;
	enum sparx5_rr_fib_type type;
	struct rhash_head ht_node; /* Router member */
	struct sparx5_rr_hw_route hw_route;
	struct sparx5_rr_nexthop_group *nh_grp;
	struct sparx5_rr_fib_info fi;
	bool trap;
	bool offload_fail;
};

struct sparx5_rr_inet6addr_event_work {
	struct work_struct work;
	struct sparx5 *sparx5;
	struct net_device *dev;
	netdevice_tracker dev_tracker;
	unsigned long event;
};

static void sparx5_rr_schedule_work(struct sparx5 *sparx5,
				    struct work_struct *work)
{
	queue_work(sparx5->router->sparx5_router_owq, work);
}

static void sparx5_rr_split_mac(unsigned char mac[ETH_ALEN], u32 split,
				u32 *msb, u32 *lsb)
{
	u32 mask = GENMASK(split - 1, 0);
	u64 m = ether_addr_to_u64(mac);

	*lsb = m & mask;
	*msb = m >> split;
}

static int sparx5_vmid_alloc(struct sparx5 *sparx5)
{
	int vmid;

	vmid = find_first_zero_bit(sparx5->router->vmid_mask,
				   sparx5->data->consts->vmid_cnt);
	if (vmid >= sparx5->data->consts->vmid_cnt)
		return -ENOMEM;

	set_bit(vmid, sparx5->router->vmid_mask);

	return vmid;
}

static void sparx5_vmid_free(struct sparx5 *sparx5, u16 vmid)
{
	clear_bit(vmid, sparx5->router->vmid_mask);
}

static void sparx5_rr_inet6_make_mask_le(int logmask, u8 *mask)
{
	/* Caller must ensure 0 <= logmask <= 128 */
	int byte_prefix = logmask / BITS_PER_BYTE;
	int rem = logmask % BITS_PER_BYTE;

	memset(mask, 0, 16);

	for (int i = 0; i < byte_prefix; i++)
		mask[15 - i] = 0xff;

	if (rem)
		mask[15 - byte_prefix] = GENMASK(7, 7 - rem + 1);
}

static int sparx5_rr_lpm_rule_xip_add(struct vcap_rule *rule,
				      struct sparx5_iaddr *addr, u32 prefix_len)
{
	struct vcap_u128_key addr_key;
	u32 mask, iaddr;

	switch (addr->version) {
	case SPARX5_IPV4:
		mask = ntohl(inet_make_mask(prefix_len));
		iaddr = ntohl(addr->ipv4);

		return vcap_rule_add_key_u32(rule, VCAP_KF_IP4_XIP, iaddr,
					     mask);
	case SPARX5_IPV6:
		sparx5_rr_inet6_make_mask_le(prefix_len, addr_key.mask);
		vcap_netbytes_copy(addr_key.value, addr->ipv6.s6_addr, 16);

		return vcap_rule_add_key_u128(rule, VCAP_KF_IP6_XIP, &addr_key);
	default:
		WARN_ON(1);
		return -EINVAL;
	}
}

static struct sparx5_rr_router_leg *
sparx5_rr_leg_find_by_dev(struct sparx5 *sparx5, struct net_device *dev)
{
	struct sparx5_rr_router_leg *leg;

	list_for_each_entry(leg, &sparx5->router->leg_list, leg_list_node) {
		if (leg->dev == dev)
			return leg;
	}

	return NULL;
}

static int sparx5_lower_dev_walk(struct net_device *lower_dev,
				 struct netdev_nested_priv *priv)
{
	int ret = 0;

	if (sparx5_netdevice_check(lower_dev)) {
		priv->data = (void *)netdev_priv(lower_dev);
		ret = 1;
	}

	return ret;
}

static struct sparx5_port *
sparx5_port_dev_lower_find_rcu(struct net_device *dev)
{
	struct netdev_nested_priv priv = {
		.data = NULL,
	};

	if (sparx5_netdevice_check(dev))
		return netdev_priv(dev);

	netdev_walk_all_lower_dev_rcu(dev, sparx5_lower_dev_walk, &priv);

	return priv.data;
}

static struct sparx5_port *sparx5_port_dev_lower_find(struct net_device *dev)
{
	struct sparx5_port *port;

	rcu_read_lock();
	port = sparx5_port_dev_lower_find_rcu(dev);
	rcu_read_unlock();

	return port;
}

static int
sparx5_rr_lpm_arp_entry_create(struct sparx5 *sparx5,
			       struct sparx5_iaddr *addr,
			       u32 prefix_len, unsigned char mac[ETH_ALEN],
			       u16 evmid, struct sparx5_rr_hw_route *hw_route)
{
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	u32 priority = LPM_SORT_KEY(prefix_len);
	struct vcap_rule *rule;
	u32 mac_msb, mac_lsb;
	int err;

	sparx5_rr_split_mac(mac, 32, &mac_msb, &mac_lsb);

	rule = vcap_alloc_rule(vctrl, pdev, VCAP_CID_PREROUTING_L0,
			       VCAP_USER_L3, priority, 0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	err = sparx5_rr_lpm_rule_xip_add(rule, addr, prefix_len);
	err |= vcap_rule_add_key_u32(rule, VCAP_KF_AFFIX, 0, 0);
	err |= vcap_rule_add_key_bit(rule, VCAP_KF_DST_FLAG, VCAP_BIT_1);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_MAC_MSB, mac_msb);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_MAC_LSB, mac_lsb);
	err |= vcap_rule_add_action_u32(rule, VCAP_AF_ARP_VMID, evmid);
	err |= vcap_rule_add_action_bit(rule, VCAP_AF_ARP_ENA, VCAP_BIT_1);

	err = err ? -EINVAL : vcap_val_add_rule(rule, LPM_PROTO(addr));
	if (!err) {
		hw_route->vrule_id = rule->id;
		hw_route->vrule_id_valid = true;
	}
	vcap_free_rule(rule);
	return err;
}

static void sparx5_rr_leg_hw_init(struct sparx5 *sparx5,
				  struct sparx5_rr_router_leg *leg)
{
	/* Associate Router leg VMID to VLAN */
	spx5_rmw(ANA_L3_VMID_CFG_VMID_SET(leg->vmid), ANA_L3_VMID_CFG_VMID,
		 sparx5, ANA_L3_VMID_CFG(leg->vid));

	/* Enable Router leg for VLAN */
	spx5_rmw(ANA_L3_VLAN_CFG_VLAN_RLEG_ENA_SET(1),
		 ANA_L3_VLAN_CFG_VLAN_RLEG_ENA, sparx5,
		 ANA_L3_VLAN_CFG(leg->vid));

	/* Configure router leg */

#if IS_ENABLED(CONFIG_IPV6)
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(1) |
		 ANA_L3_RLEG_CTRL_RLEG_EVID_SET(leg->vid) |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA_SET(1),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_EVID |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
#else
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(1) |
		 ANA_L3_RLEG_CTRL_RLEG_EVID_SET(leg->vid),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_EVID, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
#endif

	/* Configure egress VLAN in rewriter */
	spx5_rmw(REW_RLEG_CTRL_RLEG_EVID_SET(leg->vid), REW_RLEG_CTRL_RLEG_EVID,
		 sparx5, REW_RLEG_CTRL(leg->vmid));
}

static void sparx5_rr_leg_hw_deinit(struct sparx5 *sparx5,
				    struct sparx5_rr_router_leg *leg)
{
	/* Disable Router leg for VLAN */
	spx5_rmw(ANA_L3_VLAN_CFG_VLAN_RLEG_ENA_SET(0),
		 ANA_L3_VLAN_CFG_VLAN_RLEG_ENA, sparx5,
		 ANA_L3_VLAN_CFG(leg->vid));

	/* Disable IP UC routing on leg */
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(0) |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA_SET(0),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
}

static int sparx5_rr_lpm_link_local_create(struct sparx5 *sparx5)
{
	struct sparx5_iaddr addr __aligned(2) = { };
	unsigned char zero_mac[ETH_ALEN];

	eth_zero_addr(zero_mac);

	/* Trap traffic to fe80::/64 */
	addr.version = SPARX5_IPV6;
	addr.ipv6.in6_u.u6_addr8[0] = 0xfe;
	addr.ipv6.in6_u.u6_addr8[1] = 0x80;

	return sparx5_rr_lpm_arp_entry_create(sparx5, &addr,
					      SPARX5_LINK_LOCAL_PREFIX_LEN,
					      zero_mac, 0,
					      &sparx5->router->link_local);
}

static void sparx5_rr_lpm_link_local_destroy(struct sparx5 *sparx5)
{
	struct sparx5_rr_hw_route *llocal = &sparx5->router->link_local;
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;

	if (!llocal->vrule_id_valid)
		return;

	vcap_del_rule(vctrl, pdev, llocal->vrule_id);
	llocal->vrule_id_valid = false;
}

static struct sparx5_rr_router_leg *
__sparx5_rr_leg_alloc(struct sparx5 *sparx5, struct net_device *dev, u16 vmid,
		      u16 vid)
{
	struct sparx5_rr_router_leg *leg;

	leg = kzalloc_obj(*leg);
	if (!leg)
		return NULL;

	INIT_LIST_HEAD(&leg->leg_list_node);
	INIT_LIST_HEAD(&leg->neigh_list);
	leg->dev = dev;
	leg->vmid = vmid;
	leg->vid = vid;
	leg->sparx5 = sparx5;

	return leg;
}

/* Router legs are identified by their VMID in hw */
static struct sparx5_rr_router_leg *
sparx5_rr_leg_alloc(struct sparx5 *sparx5, struct net_device *dev, u16 vid)
{
	struct sparx5_rr_router_leg *leg;
	int next_vmid;

	next_vmid = sparx5_vmid_alloc(sparx5);
	if (next_vmid < 0)
		return NULL;

	leg = __sparx5_rr_leg_alloc(sparx5, dev, next_vmid, vid);
	if (!leg)
		goto err_kzalloc;

	return leg;

err_kzalloc:
	sparx5_vmid_free(sparx5, next_vmid);

	return NULL;
}

static void sparx5_rr_router_leg_destroy(struct sparx5_rr_router_leg *leg)
{
	struct sparx5 *sparx5 = leg->sparx5;

	dev_dbg(sparx5->dev, "Leg destroy vid=%u vmid=%u dev=%s\n", leg->vid,
		leg->vmid, leg->dev ? netdev_name(leg->dev) : "blackhole");

	sparx5_rr_leg_hw_deinit(sparx5, leg);
	sparx5_vmid_free(leg->sparx5, leg->vmid);
	list_del(&leg->leg_list_node);

	if (leg->dev) {
		if (atomic_dec_return(&sparx5->router->legs_count) == 0)
			sparx5_rr_lpm_link_local_destroy(sparx5);

		netdev_put(leg->dev, &leg->dev_tracker);
	}
	kfree(leg);
}

static struct sparx5_rr_router_leg *
sparx5_rr_router_leg_create(struct sparx5 *sparx5, struct net_device *dev,
			    u16 vid)
{
	struct sparx5_rr_router_leg *leg;

	leg = sparx5_rr_leg_alloc(sparx5, dev, vid);
	if (!leg)
		return ERR_PTR(-ENOMEM);

	/* Prevent net device from being freed while we have added it to a
	 * router leg.
	 */
	netdev_hold(dev, &leg->dev_tracker, GFP_KERNEL);

	/* While a router leg exists, add route to trap link-local traffic. */
	if (atomic_inc_return(&sparx5->router->legs_count) == 1) {
		if (sparx5_rr_lpm_link_local_create(sparx5))
			dev_warn(sparx5->dev,
				 "Failed to create link-local route\n");
	}

	list_add(&leg->leg_list_node, &sparx5->router->leg_list);
	sparx5_rr_leg_hw_init(sparx5, leg);

	dev_dbg(sparx5->dev, "Leg create dev=%s vid=%u vmid=%u\n", dev->name,
		leg->vid, leg->vmid);

	return leg;
}

static bool sparx5_rr_dev_real_is_vlan_aware(struct net_device *dev)
{
	struct net_device *vlan_rdev;
	/* Support l3 offloading for:
	 *	1) upper vlan interfaces for the bridge.
	 */
	if (is_vlan_dev(dev)) {
		if (netif_is_bridge_port(dev))
			return false;

		vlan_rdev = vlan_dev_real_dev(dev);
		if (sparx5_netdevice_check(vlan_rdev))
			return false;

		return netif_is_bridge_master(vlan_rdev) &&
		       br_vlan_enabled(vlan_rdev) &&
		       sparx5_port_dev_lower_find(vlan_rdev);
	}

	return false;
}

static void sparx5_rr_leg_base_mac_set(struct sparx5 *sparx5,
				       unsigned char mac[ETH_ALEN])
{
	u8 rleg_type_sel = SPARX5_RLEG_USE_GLOBAL_BASE_MAC;
	u32 mac_msb, mac_lsb;

	sparx5_rr_split_mac(mac, 24, &mac_msb, &mac_lsb);

	dev_dbg(sparx5->dev, "Router leg base MAC=%pM\n", mac);

	/* The global router leg MAC must be set consistently across ANA_L3, REW
	 * and EACL.
	 */
	spx5_wr(ANA_L3_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		ANA_L3_RLEG_CFG_0);

	spx5_rmw(ANA_L3_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_MSB |
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, ANA_L3_RLEG_CFG_1);

	/* Set global Router leg MAC (REW) */
	spx5_wr(REW_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		REW_RLEG_CFG_0);

	spx5_rmw(REW_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 REW_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 REW_RLEG_CFG_1_RLEG_MAC_MSB | REW_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, REW_RLEG_CFG_1);

	/* Set global Router leg MAC (EACL) */
	spx5_wr(EACL_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		EACL_RLEG_CFG_0);

	spx5_rmw(EACL_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 EACL_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 EACL_RLEG_CFG_1_RLEG_MAC_MSB |
		 EACL_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, EACL_RLEG_CFG_1);
}

static bool
sparx5_rr_router_leg_addr_list_empty_rcu(struct sparx5_rr_router_leg *leg)
{
	struct inet6_dev *inet6_dev;
	struct in_device *in_dev;

	in_dev = __in_dev_get_rcu(leg->dev);
	if (in_dev && rcu_access_pointer(in_dev->ifa_list))
		return false;

	inet6_dev = __in6_dev_get(leg->dev);
	if (inet6_dev && !list_empty(&inet6_dev->addr_list))
		return false;

	return true;
}

static bool
sparx5_rr_router_leg_addr_list_empty(struct sparx5_rr_router_leg *leg)
{
	bool addr_list_empty;

	rcu_read_lock();
	addr_list_empty = sparx5_rr_router_leg_addr_list_empty_rcu(leg);
	rcu_read_unlock();

	return addr_list_empty;
}

static int __sparx5_rr_inetaddr_event(struct sparx5 *sparx5,
				      struct net_device *dev,
				      unsigned long event)
{
	struct sparx5_rr_router_leg *leg;
	u16 vid;

	if (!sparx5_rr_dev_real_is_vlan_aware(dev))
		return 0;

	/* Our basic case: ip addr/subnet added to vlan upper of
	 * bridge dev.
	 */
	switch (event) {
	case NETDEV_UP:
		leg = sparx5_rr_leg_find_by_dev(sparx5, dev);
		if (leg)
			return 0;

		/* HW allows at most 1 leg per VLAN, but we do not need to
		 * lookup leg by vid, since the kernel does not allow multiple
		 * vlan devs with the same vid on top of a given device.
		 */
		vid = vlan_dev_vlan_id(dev);

		leg = sparx5_rr_router_leg_create(sparx5, dev, vid);
		if (IS_ERR(leg))
			return PTR_ERR(leg);
		break;
	case NETDEV_DOWN:
		leg = sparx5_rr_leg_find_by_dev(sparx5, dev);
		if (!leg || !sparx5_rr_router_leg_addr_list_empty(leg))
			return 0;

		sparx5_rr_router_leg_destroy(leg);
		break;
	}

	return 0;
}

static int sparx5_rr_inetaddr_event_handle(struct sparx5 *sparx5,
					   struct net_device *dev,
					   unsigned long event)
{
	int err;

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	mutex_lock(&sparx5->router->lock);
	err = __sparx5_rr_inetaddr_event(sparx5, dev, event);
	mutex_unlock(&sparx5->router->lock);

	return notifier_from_errno(err);
}

/* Called with RTNL. */
static int sparx5_rr_inet6addr_valid_event(struct notifier_block *nb,
					   unsigned long event, void *ptr)
{
	struct in6_validator_info *i6vi = (struct in6_validator_info *)ptr;
	struct net_device *dev = i6vi->i6vi_dev->dev;
	struct sparx5_router *router;

	ASSERT_RTNL();

	if (event != NETDEV_UP)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inet6addr_valid_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

static void sparx5_rr_inet6addr_event_work(struct work_struct *work)
{
	struct sparx5_rr_inet6addr_event_work *addr_work =
		container_of(work, struct sparx5_rr_inet6addr_event_work, work);
	struct sparx5_router *router = addr_work->sparx5->router;

	rtnl_lock();
	mutex_lock(&router->lock);

	__sparx5_rr_inetaddr_event(addr_work->sparx5, addr_work->dev,
				   addr_work->event);

	mutex_unlock(&router->lock);
	rtnl_unlock();
	netdev_put(addr_work->dev, &addr_work->dev_tracker);
	kfree(addr_work);
}

/* Called in atomic context. */
static int sparx5_rr_inet6addr_event(struct notifier_block *nb,
				     unsigned long event, void *ptr)
{
	struct inet6_ifaddr *if6 = (struct inet6_ifaddr *)ptr;
	struct sparx5_rr_inet6addr_event_work *work;
	struct net_device *dev = if6->idev->dev;
	struct sparx5_router *router;

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	work = kzalloc_obj(*work, GFP_ATOMIC);
	if (!work)
		return NOTIFY_BAD;

	router = container_of(nb, struct sparx5_router, inet6addr_nb);
	INIT_WORK(&work->work, sparx5_rr_inet6addr_event_work);
	work->sparx5 = router->sparx5;
	work->dev = dev;
	work->event = event;
	netdev_hold(dev, &work->dev_tracker, GFP_ATOMIC);
	sparx5_rr_schedule_work(router->sparx5, &work->work);

	return NOTIFY_DONE;
}

/* Handle events for ip address changes on ifs. Used to manage router legs.
 * Called with RTNL.
 */
static int sparx5_rr_inetaddr_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = (struct in_ifaddr *)ptr;
	struct net_device *dev = ifa->ifa_dev->dev;
	struct sparx5_router *router;

	ASSERT_RTNL();

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inetaddr_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

/* Called with RTNL. */
static int sparx5_rr_inetaddr_valid_event(struct notifier_block *nb,
					  unsigned long event, void *ptr)
{
	struct in_validator_info *ivi = (struct in_validator_info *)ptr;
	struct net_device *dev = ivi->ivi_dev->dev;
	struct sparx5_router *router;

	ASSERT_RTNL();

	if (event != NETDEV_UP)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inetaddr_valid_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

/* Called with RTNL. */
static int sparx5_rr_netdevice_event(struct notifier_block *nb,
				     unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	unsigned char mac[ETH_ALEN] __aligned(2);
	struct sparx5_rr_router_leg *leg;
	struct sparx5_router *router;
	struct sparx5 *sparx5;

	ASSERT_RTNL();

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_OK;

	router = container_of(nb, struct sparx5_router, netdevice_nb);
	sparx5 = router->sparx5;

	switch (event) {
	case NETDEV_CHANGEADDR:
		/* Allow single bridge. Global router leg MAC tracks bridge mac. */
		if (netif_is_bridge_master(dev) && sparx5_port_dev_lower_find(dev)) {
			ether_addr_copy(mac, dev->dev_addr);
			sparx5_rr_leg_base_mac_set(sparx5, mac);
		}
		break;
	case NETDEV_UNREGISTER:
		mutex_lock(&router->lock);
		leg = sparx5_rr_leg_find_by_dev(sparx5, dev);
		if (leg)
			sparx5_rr_router_leg_destroy(leg);
		mutex_unlock(&router->lock);
		break;
	}

	return NOTIFY_OK;
}

static int sparx5_rr_blackhole_leg_create(struct sparx5 *sparx5)
{
	struct sparx5_rr_router_leg *leg;
	u16 vmid, vid;

	vmid = SPARX5_BLACKHOLE_VMID(sparx5);
	vid = SPARX5_BLACKHOLE_VID;

	leg = __sparx5_rr_leg_alloc(sparx5, NULL, vmid, vid);
	if (!leg)
		return -ENOMEM;

	set_bit(vmid, sparx5->router->vmid_mask);

	list_add(&leg->leg_list_node, &sparx5->router->leg_list);
	sparx5_rr_leg_hw_init(sparx5, leg);

	dev_dbg(sparx5->dev, "Blackhole leg create vid=%u vmid=%u\n",
		leg->vid, leg->vmid);

	return 0;
}

static void sparx5_rr_router_legs_flush(struct sparx5 *sparx5)
{
	struct sparx5_rr_router_leg *leg, *tmp;

	list_for_each_entry_safe(leg, tmp, &sparx5->router->leg_list,
				 leg_list_node)
		sparx5_rr_router_leg_destroy(leg);
}

int sparx5_rr_router_init(struct sparx5 *sparx5)
{
	struct sparx5_router *r;
	int err;

	r = kzalloc_obj(*sparx5->router);
	if (!r)
		return -ENOMEM;

	mutex_init(&r->lock);
	sparx5->router = r;
	r->sparx5 = sparx5;

	INIT_LIST_HEAD(&r->leg_list);

	/* Add reserved leg for blackhole routes. */
	err = sparx5_rr_blackhole_leg_create(sparx5);
	if (err)
		goto err_free_router;

	r->sparx5_router_owq = alloc_ordered_workqueue("sparx5_router_owq", 0);
	if (!r->sparx5_router_owq) {
		err = -ENOMEM;
		goto err_blackhole_destroy;
	}

	atomic_set(&r->legs_count, 0);
	r->link_local.vrule_id = 0;
	r->link_local.vrule_id_valid = false;
	/* VCAP API requires a port net_device, to get a sparx5 reference.
	 * Fetch any valid port.
	 */
	for (int i = 0; i < sparx5->data->consts->n_ports; i++) {
		if (!sparx5->ports[i])
			continue;

		r->port_dev = sparx5->ports[i]->ndev;
		if (r->port_dev)
			break;
	}
	if (!r->port_dev) {
		err = -ENXIO;
		goto err_workqueue_destroy;
	}

	/* Enable L3 UC routing on all ports. */
	spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA);
	if (is_sparx5(sparx5)) {
		spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA1);
		spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA2);
	}

	/* Enable routing and global router options */
	spx5_rmw(ANA_L3_ROUTING_CFG_L3_ENA_MODE_SET(1) |
		 ANA_L3_ROUTING_CFG_RT_SMAC_UPDATE_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_RLEG_IP_HDR_FAIL_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_IP4_OPTIONS_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_IP6_HOPBYHOP_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_IP6_HC_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_IP4_TTL_REDIR_ENA_SET(1),
		 ANA_L3_ROUTING_CFG_L3_ENA_MODE |
		 ANA_L3_ROUTING_CFG_RT_SMAC_UPDATE_ENA |
		 ANA_L3_ROUTING_CFG_CPU_RLEG_IP_HDR_FAIL_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_CPU_IP4_OPTIONS_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_CPU_IP6_HOPBYHOP_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_IP6_HC_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_IP4_TTL_REDIR_ENA,
		 sparx5, ANA_L3_ROUTING_CFG);

	/* By default, routing related frame edits are done in REW, but when
	 * combining routing with PTP, ANA_ACL must be configured to change DMAC
	 * to next-hop DMAC in order to allow other information to be stored in
	 * the IFH.
	 *
	 * This enables routing related frame edits independently of VCAP_S2
	 * action ACL_RT_MODE.
	 */
	spx5_rmw(ANA_ACL_VCAP_S2_MISC_CTRL_ACL_RT_SEL_SET(1),
		 ANA_ACL_VCAP_S2_MISC_CTRL_ACL_RT_SEL, sparx5,
		 ANA_ACL_VCAP_S2_MISC_CTRL);

	r->inetaddr_nb.notifier_call = sparx5_rr_inetaddr_event;
	err = register_inetaddr_notifier(&r->inetaddr_nb);
	if (err)
		goto err_workqueue_destroy;

	r->inetaddr_valid_nb.notifier_call = sparx5_rr_inetaddr_valid_event;
	err = register_inetaddr_validator_notifier(&r->inetaddr_valid_nb);
	if (err)
		goto err_unreg_inet_notifier;

	r->netdevice_nb.notifier_call = sparx5_rr_netdevice_event;
	err = register_netdevice_notifier(&r->netdevice_nb);
	if (err)
		goto err_unreg_inet_addr_val_notifier;

	r->inet6addr_valid_nb.notifier_call = sparx5_rr_inet6addr_valid_event;
	err = register_inet6addr_validator_notifier(&r->inet6addr_valid_nb);
	if (err)
		goto err_unreg_netdev_notifier;

	r->inet6addr_nb.notifier_call = sparx5_rr_inet6addr_event;
	err = register_inet6addr_notifier(&r->inet6addr_nb);
	if (err)
		goto err_unreg_inet6_addr_val_notifier;

	return 0;

err_unreg_inet6_addr_val_notifier:
	unregister_inet6addr_validator_notifier(&r->inet6addr_valid_nb);
err_unreg_netdev_notifier:
	unregister_netdevice_notifier(&r->netdevice_nb);
err_unreg_inet_addr_val_notifier:
	unregister_inetaddr_validator_notifier(&r->inetaddr_valid_nb);
err_unreg_inet_notifier:
	unregister_inetaddr_notifier(&r->inetaddr_nb);
err_workqueue_destroy:
	destroy_workqueue(r->sparx5_router_owq);
err_blackhole_destroy:
	sparx5_rr_router_legs_flush(sparx5);
err_free_router:
	mutex_destroy(&r->lock);
	kfree(r);

	return err;
}

void sparx5_rr_router_deinit(struct sparx5 *sparx5)
{
	struct sparx5_router *router = sparx5->router;

	unregister_inet6addr_notifier(&router->inet6addr_nb);
	unregister_inet6addr_validator_notifier(&router->inet6addr_valid_nb);
	unregister_netdevice_notifier(&router->netdevice_nb);
	unregister_inetaddr_validator_notifier(&router->inetaddr_valid_nb);
	unregister_inetaddr_notifier(&router->inetaddr_nb);
	destroy_workqueue(router->sparx5_router_owq);
	sparx5_rr_router_legs_flush(sparx5);
	mutex_destroy(&router->lock);
	kfree(router);
}
