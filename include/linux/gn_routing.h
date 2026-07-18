/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_GN_ROUTING_H__
#define __LINUX_GN_ROUTING_H__

#include <linux/gn.h>
#include <linux/types.h>
#include <linux/if_ether.h>

#define GN_MAX_PDR_EMA_BETA 90
#define GN_MAX_PDR 100
#define GN_DPL_SIZE 8
#define GN_LSB_SIZE 16	// this is the number of entries per entry (has to be a power of 2 atm)
			// ETSI sets a maximum of 1024 octets/bytes, which is not trivial to maintain

#define GN_NON_AREA_FORWARDING_UNSPECIFIED	0
#define GN_NON_AREA_FORWARDING_GREEDY		1
#define GN_NON_AREA_FORWARDING_CBF		2
#define GN_NON_AREA_FORWARDING GN_NON_AREA_FORWARDING_GREEDY

#define GN_AREA_FORWARDING_UNSPECIFIED	0
#define GN_AREA_FORWARDING_SIMPLE	1
#define GN_AREA_FORWARDING_CBF		2
#define GN_AREA_FORWARDING_ADVANCED	3
#define GN_AREA_FORWARDING GN_AREA_FORWARDING_SIMPLE

#define GN_QUEUE_DIRECT		0
#define GN_QUEUE_LS_PENDING	1
#define GN_QUEUE_LS_STALE	2
#define GN_QUEUE_ERROR		3

#define GN_FORWARD_BROADCAST	0
#define GN_FORWARD_NEXT_HOP	1
#define GN_FORWARD_BUFFER	2
#define GN_FORWARD_DISCARD	3

struct gn_dpd_buf {
	u16 sn[GN_DPL_SIZE];
	u8 head;
};

struct loc_te {
	gn_address_t addr;
	u8 ll_address[ETH_ALEN];
	struct gn_lpv pv;
	struct gn_dpd_buf dpl;
	struct sk_buff_head lsb;
	u32 tst_addr;
	u32 pdr;
	struct hlist_node hnode;
	u8 ls_pending: 1;
	u8 is_neighbour: 1;
};

s64 gn_F(struct gn_coord self, struct gn_geo_scope scope);
int gn_gxc_forward(struct gn_iface *gnif, s64 f, u8 *addr, struct gn_lpv *depv);

struct gn_iface *gn_find_interface(gn_address_t addr);
struct gn_iface *gn_find_interface_by_dev(struct net_device *dev);
int gn_query_ll_address(gn_address_t addr, u8 *ll_address);
int gn_query_ll_nexthop(struct gn_iface *gnif, gn_address_t query_addr, u8 *ll_address);
int gn_fill_depv(struct gn_spv *depv, gn_address_t dest_addr);
int gn_ls_queue(gn_address_t dest_addr, struct sk_buff *skb);
void gn_ls_flush(gn_address_t dest_addr);
int gn_update_location_table(struct gn_lpv *pv, bool make_neighbour, const u8 *ll_address, const __be16 *sn);
void gn_routing_exit(void);

#endif // __LINUX_GN_ROUTING_H__
