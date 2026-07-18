// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__
#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <net/datalink.h>

#include <linux/gn_routing.h>
#include <linux/gn.h>

extern struct datalink_proto *gn_dl;

static DEFINE_HASHTABLE(gn_loc_t, 3);
static DEFINE_SPINLOCK(gn_loc_t_lock);

// ###### values in meters
#define EARTH_RADIUS 6378388ULL
#define PI 31415926ULL // * 10^7
#define METER_PER_LON 111317ULL
#define RAD_PER_DEGREE 174533ULL // * 10^7 - same as long and lat from PV

#define GN_LT_JIFFIES msecs_to_jiffies(GN_LOC_TE_LIFETIME)
#define GN_TST_VALID(tst) \
	time_before(jiffies, (unsigned long)(tst) + GN_LT_JIFFIES)

/* GeoNetworking routing */

static u16 *gn_dpd_find(struct gn_dpd_buf *buf, u16 sn)
{
	int i = 0;

	for (; i < GN_DPL_SIZE; i++) {
		if (buf->sn[i] == sn)
			return &buf->sn[i];
	}
	return NULL;
}

static void gn_dpd_insert(struct gn_dpd_buf *buf, u16 sn)
{
	buf->sn[buf->head] = sn;
	buf->head = (buf->head + 1) % GN_DPL_SIZE;
}

static u32 pdr(u32 old_pdr, u32 delta)
{
	if (!delta)
		delta = 1;
	return (GN_MAX_PDR_EMA_BETA * old_pdr) / 100 +
	       (((100 - GN_MAX_PDR_EMA_BETA) * (100 / delta)) / 100);
}

// table is * 1000 | p1 * 100 entry
static const int cos_table[] = {
	100000, 99995,	99980,	99955,	99920,	99875,	99820,	99755,	99680,
	99595,	99500,	99396,	99281,	99156,	99022,	98877,	98723,	98558,
	98384,	98200,	98007,	97803,	97590,	97367,	97134,	96891,	96639,
	96377,	96106,	95824,	95534,	95233,	94924,	94604,	94275,	93937,
	93590,	93233,	92866,	92491,	92106,	91712,	91309,	90897,	90475,
	90045,	89605,	89157,	88699,	88233,	87758,	87274,	86782,	86281,
	85771,	85252,	84726,	84190,	83646,	83094,	82534,	81965,	81388,
	80803,	80210,	79608,	78999,	78382,	77757,	77125,	76484,	75836,
	75181,	74517,	73847,	73169,	72484,	71791,	71091,	70385,	69671,
	68950,	68222,	67488,	66746,	65998,	65244,	64483,	63715,	62941,
	62161,	61375,	60582,	59783,	58979,	58168,	57352,	56530,	55702,
	54869,	54030,	53186,	52337,	51482,	50622,	49757,	48887,	48012,
	47133,	46249,	45360,	44466,	43568,	42666,	41759,	40849,	39934,
	39015,	38092,	37166,	36236,	35302,	34365,	33424,	32480,	31532,
	30582,	29628,	28672,	27712,	26750,	25785,	24818,	23848,	22875,
	21901,	20924,	19945,	18964,	17981,	16997,	16010,	15023,	14033,
	13042,	12050,	11057,	10063,	9067,	8071,	7074,	6076,	5077,
	4079,	3079,	2079,	1080,	0,	-920,	-1920,	-2920,	-3919,
	-4918,	-5917,	-6915,	-7912,	-8909,	-9904,	-10899, -11892, -12884,
	-13875, -14865, -15853, -16840, -17825, -18808, -19789, -20768, -21745,
	-22720, -23693, -24663, -25631, -26596, -27559, -28519, -29476, -30430,
	-31381, -32329, -33274, -34215, -35153, -36087, -37018, -37945, -38868,
	-39788, -40703, -41615, -42522, -43425, -44323, -45218, -46107, -46992,
	-47873, -48748, -49619, -50485, -51345, -52201, -53051, -53896, -54736,
	-55570, -56399, -57221, -58039, -58850, -59656, -60455, -61249, -62036,
	-62817, -63592, -64361, -65123, -65879, -66628, -67370, -68106, -68834,
	-69556, -70271, -70979, -71680, -72374, -73060, -73739, -74411, -75075,
	-75732, -76382, -77023, -77657, -78283, -78901, -79512, -80114, -80709,
	-81295, -81873, -82444, -83005, -83559, -84104, -84641, -85169, -85689,
	-86200, -86703, -87197, -87682, -88158, -88626, -89085, -89534, -89975,
	-90407, -90830, -91244, -91648, -92044, -92430, -92807, -93175, -93533,
	-93883, -94222, -94553, -94873, -95185, -95486, -95779, -96061, -96334,
	-96598, -96852, -97096, -97330, -97555, -97770, -97975, -98170, -98356,
	-98531, -98697, -98853, -98999, -99135, -99262, -99378, -99484, -99581,
	-99667, -99744, -99810, -99867, -99914, -99950, -99977, -99993, -100000
};

/* icos() - look up in cos_table for a rad value.
 * @rad : the rad value.* 10^7
 *
 * Return : the cosine value * 100000
 */
static int icos(__s64 rad)
{
	size_t idx;

	if (rad < 0)
		rad = -rad;

	if (rad >= 2 * PI)
		rad %= (2 * PI);

	if (rad > PI)
		rad = 2 * PI - rad;

	idx = rad / 100000;
	if (idx >= ARRAY_SIZE(cos_table))
		idx = ARRAY_SIZE(cos_table) - 1;

	return cos_table[idx];
}

/* degree_to_rad() - convert a degree value to a rad value.
 * @a : the degree value as 1/10 micro degree (10^7).
 *
 * Return : the rad value * 10^7.
 */
static __s64 degree_to_rad(__s64 a)
{
	return (((RAD_PER_DEGREE * a) / 10000000ULL)) % (PI * 2ULL);
}

/* diff() - calculate the difference beween a and b.
 * @a : value a.
 * @b : value b.
 *
 * Return : the difference
 */
static __s32 diff(__s32 a, __s32 b)
{
	__s32 r = a - b;

	return r < 0 ? r * -1 : r;
}

/* get_distance() - calculate the distance of to position vectors
 * center/self: positionvectors, whose distance is calculated
 * @x : after execute includes the meter on X-axes.
 * @y : after execute includes the meter on Y-axes.
 *
 * the calculation based on pythagoras.
 */
static struct gn_coord gn_coord_diff(struct gn_coord lhs, struct gn_coord rhs)
{
	struct gn_coord c;
	s32 lat;

	lat = degree_to_rad((lhs.lat + rhs.lat) / 2);

	c.lat = (METER_PER_LON * icos(lat) * diff(rhs.lon, lhs.lon)) /
		(10000000ULL * 100000ULL);
	c.lon = (METER_PER_LON * diff(rhs.lat, lhs.lat)) / 10000000ULL;
	return c;
}

static inline struct gn_coord pv_to_coord(struct gn_lpv *pv)
{
	struct gn_coord c = {
		.lat = be32_to_cpu(pv->lat),
		.lon = be32_to_cpu(pv->lon),
	};
	return c;
}

static inline u64 dist(struct gn_coord c)
{
	return c.lat * c.lat + c.lon * c.lon;
}

/* gn_F() - decides if self is inside or at the border of the geographical area.
 * @center: The position vector of the Package sender.
 * @self: The own position vector.
 * @t : The Type of geographical area.
 * @r : The radius of area. Only use for circle.
 * @a : The width of area. Only use for rectangel and elipse.
 * @b : The height of area. Only use for rectangel and elipse.
 * @angel : ???
 *
 * Return:  if the result is > 0 self is inside area.
			if the result is 0 self is on border of area.
			otherwise self is outside of area.
 */
__s64 gn_F(struct gn_coord self, struct gn_geo_scope scope)
{
	struct gn_coord coord_diff = gn_coord_diff(self, scope.coord);
	s32 a2, b2, x2, y2;
	s64 result = -1;
	/* Note: Scope angle rotation for non-circular geographical areas */
	a2 = scope.a * scope.a;
	b2 = scope.b * scope.b;
	x2 = coord_diff.lat * coord_diff.lat;
	y2 = coord_diff.lon * coord_diff.lon;

	switch (scope.shape) {
	case GN_SHAPE_CIRCLE:
		result = a2 - (x2 + y2);
		break;
	case GN_SHAPE_RECTANGLE:
		result = a2 - x2;
		if (result > (b2 - y2))
			result = b2 - y2;
		break;
	case GN_SHAPE_ELLIPSE:
		if (scope.a == 0 || scope.b == 0) {
			pr_debug("internal error: input out of range");
			break;
		}
		result = 1000;
		result -= (y2 * 1000) / a2;
		result -= (x2 * 1000) / b2;
		break;
	}

	return result;
}

static int greedy_forward(struct gn_iface *gnif, u8 *addr, struct gn_lpv *depv)
{
	struct gn_coord dest_coord = pv_to_coord(depv);
	u64 min_dist, curr_dist, ego_dist;
	u8 *found_addr = NULL;
	struct loc_te *curr;
	int bkt, rc;

	ego_dist = dist(gn_coord_diff(dest_coord, gnif->pos.coord));
	min_dist = ego_dist;
	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each(gn_loc_t, bkt, curr, hnode) {
		if (curr->is_neighbour) {
			curr_dist = dist(gn_coord_diff(dest_coord,
						       pv_to_coord(&curr->pv)));
			if (curr_dist < min_dist) {
				min_dist = curr_dist;
				found_addr = curr->ll_address;
			}
		}
	}
	/* Note: Traffic class and store-carry-forward evaluation for next-hop selection */
	if (found_addr) {
		ether_addr_copy(addr, found_addr);
		rc = GN_FORWARD_NEXT_HOP;
	} else {
		rc = GN_FORWARD_BROADCAST;
	}
	spin_unlock_bh(&gn_loc_t_lock);

	return rc;
}

int gn_gxc_forward(struct gn_iface *gnif, s64 f, u8 *addr, struct gn_lpv *depv)
{
	if (f >= 0) {
		// Area forwarding
		switch (GN_AREA_FORWARDING) {
		case GN_AREA_FORWARDING_SIMPLE:
			return GN_FORWARD_BROADCAST;
		default:
			pr_warn("non-simple area forwarding not implemented");
			return GN_FORWARD_BROADCAST;
		}
	} else {
		// Non-area forwarding
		switch (GN_NON_AREA_FORWARDING) {
		case GN_NON_AREA_FORWARDING_GREEDY:
			return greedy_forward(gnif, addr, depv);
		default:
			return GN_FORWARD_BROADCAST;
		}
	}
}

static void debug_loc_te(void)
{
	struct hlist_node *tmp;
	struct loc_te *entry;
	int bucket;

	spin_lock_bh(&gn_loc_t_lock);
	if (hash_empty(gn_loc_t)) {
		spin_unlock_bh(&gn_loc_t_lock);
		return;
	}

	pr_debug("Printing location table\n");
	hash_for_each_safe(gn_loc_t, bucket, tmp, entry, hnode) {
		pr_debug(
			"LOC_TE(%p) tst=%x addr=%llx ll_addr=%llx is_neighbour=%x ls_pending=%x\n",
			entry, entry->tst_addr, be64_to_cpu(entry->addr),
			ether_addr_to_u64(entry->ll_address),
			entry->is_neighbour, entry->ls_pending);
	}
	spin_unlock_bh(&gn_loc_t_lock);
}

static void gn_prune(void)
{
	struct hlist_node *tmp;
	struct loc_te *entry;
	int bucket;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_safe(gn_loc_t, bucket, tmp, entry, hnode) {
		if (!GN_TST_VALID(entry->tst_addr)) {
			pr_debug("pruning entry addr=%llx\n", entry->addr);
			skb_queue_purge(&entry->lsb);
			hash_del(&entry->hnode);
			kfree(entry);
		}
	}
	spin_unlock_bh(&gn_loc_t_lock);
}

/* update_location_table() - update location table
 * @pv: the position vector which indicate an entry.
 *
 * Return: 0 on success, negative errno on error, -EALREADY if packet is duplicate.
 *
 * Update an entry, which indicated by @spv. If no entry found its will be add a new one.
 * And all entries will be check with the update function.
 */
int gn_update_location_table(struct gn_lpv *pv, bool make_neighbour,
			     const u8 *ll_address, const __be16 *sn)
{
	/* ETSI EN 302 636-4-1 Clause C.2: Update PV only when incoming PV timestamp is newer */
	struct loc_te *entry;
	bool found = false;

	if (gn_find_interface(pv->addr))
		return -EINVAL;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, pv->addr) {
		if (entry->addr != pv->addr)
			continue;

		found = true;

		pr_debug("updating entry addr=%llx\n", pv->addr);

		entry->pdr = pdr(entry->pdr,
				 jiffies_to_msecs(jiffies) -
					 jiffies_to_msecs(entry->tst_addr));
		entry->tst_addr = jiffies;
		if (make_neighbour) {
			if (!ll_address) {
				spin_unlock_bh(&gn_loc_t_lock);
				return -EINVAL;
			}
			ether_addr_copy(entry->ll_address, ll_address);
			entry->is_neighbour = true;
		}
		memcpy(&entry->pv, pv, sizeof(*pv));
		if (sn) {
			// Perform DPD
			if (gn_dpd_find(&entry->dpl, be16_to_cpu(*sn))) {
				pr_debug("received duplicate packet\n");
				spin_unlock_bh(&gn_loc_t_lock);
				return -EALREADY;
			}
			gn_dpd_insert(&entry->dpl, be16_to_cpu(*sn));
		}
		break;
	}

	if (!found) {
		entry = kzalloc_obj(*entry, GFP_ATOMIC);
		if (!entry) {
			spin_unlock_bh(&gn_loc_t_lock);
			return -ENOMEM;
		}
		pr_debug("adding entry addr=%llx\n", pv->addr);
		entry->addr = pv->addr;
		entry->tst_addr = jiffies;
		entry->is_neighbour = make_neighbour;
		memcpy(&entry->pv, pv, sizeof(*pv));
		skb_queue_head_init(&entry->lsb);
		if (make_neighbour) {
			if (!ll_address) {
				kfree(entry);
				spin_unlock_bh(&gn_loc_t_lock);
				return -EINVAL;
			}
			ether_addr_copy(entry->ll_address, ll_address);
		}
		if (sn)
			gn_dpd_insert(&entry->dpl, be16_to_cpu(*sn));

		hash_add(gn_loc_t, &entry->hnode, entry->addr);
	}
	spin_unlock_bh(&gn_loc_t_lock);

	gn_prune();

	debug_loc_te();

	return 0;
}

static void __ls_queue(struct sk_buff_head *q, struct sk_buff *skb)
{
	u32 qlen = skb_queue_len(q);
	struct sk_buff *curr;

	while (qlen-- > GN_LSB_SIZE) {
		curr = skb_dequeue(q);
		if (curr)
			kfree_skb(curr);
	}

	skb_queue_tail(q, skb);
}

/* ls_queue() - queue packet for delivery if destination is not a neighbour
 * @dest_addr: destination address
 * @skb: the packet
 *
 * If the destination address is not a known neighbour ẃith recent activity,
 * queue the packet
 *
 * Return: GN_QUEUE_DIRECT if the destination is a neighbour,
 * GN_QUEUE_LS_PENDING if the destination is unknown, but a LS request is
 * pending and the packet is queued,
 * GN_QUEUE_LS_STALE if the destination is unknown or its entry is stale and
 * we should send a LS query, GN_QUEUE_ERROR on error
 */
int gn_ls_queue(gn_address_t dest_addr, struct sk_buff *skb)
{
	struct loc_te *entry;
	int rc = -ENOENT;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, dest_addr) {
		if (entry->addr != dest_addr)
			continue;

		if (GN_TST_VALID(entry->tst_addr)) {
			// Entry is valid, no need for LS
			rc = GN_QUEUE_DIRECT;
		} else if (entry->ls_pending == 1) {
			// Entry is stale, but we already sent a LS request
			rc = GN_QUEUE_LS_PENDING;
			__ls_queue(&entry->lsb, skb);
		} else {
			// Entry is stale, perform LS request
			rc = GN_QUEUE_LS_STALE;

			entry->ls_pending = 1;
			__ls_queue(&entry->lsb, skb);
		}

		break;
	}
	if (rc == -ENOENT) {
		entry = kzalloc_obj(*entry, GFP_ATOMIC);
		if (!entry) {
			spin_unlock_bh(&gn_loc_t_lock);
			return -ENOMEM;
		}

		rc = GN_QUEUE_LS_STALE;
		entry->addr = dest_addr;
		entry->ls_pending = 1;
		skb_queue_head_init(&entry->lsb);
		__ls_queue(&entry->lsb, skb);

		hash_add(gn_loc_t, &entry->hnode, entry->addr);
	}
	spin_unlock_bh(&gn_loc_t_lock);

	return rc;
}

void gn_ls_flush(gn_address_t dest_addr)
{
	struct sk_buff *tmp_skb;
	u8 ll_address[ETH_ALEN];
	struct loc_te *entry;
	bool has_mac = false;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, dest_addr) {
		if (entry->addr != dest_addr)
			continue;
		if (!entry->ls_pending)
			break;

		if (entry->is_neighbour &&
		    !is_zero_ether_addr(entry->ll_address)) {
			ether_addr_copy(ll_address, entry->ll_address);
			has_mac = true;
		}

		while ((tmp_skb = skb_dequeue(&entry->lsb)) != NULL) {
			struct gn_header *gh =
				(struct gn_header *)skb_network_header(tmp_skb);
			struct gn_iface *gnif =
				gn_find_interface_by_dev(tmp_skb->dev);

			/* Populate DEPV for queued GeoUnicast packets when location is
			   resolved */
			if (gh->gc_h.ht == CH_HT_GUC) {
				gh->guc_h.depv.tst = entry->pv.tst;
				gh->guc_h.depv.lat = entry->pv.lat;
				gh->guc_h.depv.lon = entry->pv.lon;
			}

			if (has_mac)
				gn_dl->request(gn_dl, tmp_skb, ll_address);
			else if (gnif && !gn_query_ll_nexthop(gnif, dest_addr,
							      ll_address))
				gn_dl->request(gn_dl, tmp_skb, ll_address);
			else
				gn_dl->request(gn_dl, tmp_skb,
					       tmp_skb->dev->broadcast);
		}
		entry->ls_pending = 0;
		break;
	}
	spin_unlock_bh(&gn_loc_t_lock);
}

int gn_query_ll_address(gn_address_t query_addr, u8 *ll_address)
{
	struct loc_te *entry;
	int rc = -ENOENT;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, query_addr) {
		if (entry->addr != query_addr)
			continue;
		if (!entry->is_neighbour || !GN_TST_VALID(entry->tst_addr))
			break;
		if (is_zero_ether_addr(entry->ll_address))
			break;

		rc = 0;
		ether_addr_copy(ll_address, entry->ll_address);
		break;
	}
	spin_unlock_bh(&gn_loc_t_lock);

	return rc;
}

/**
 * gn_query_ll_nexthop - Query linklayer address or next-hop for GUC forwarding
 * @gnif: Local GeoNetworking interface sending the packet
 * @query_addr: Destination GeoNetworking address
 * @ll_address: Buffer to receive the linklayer (MAC) address
 *
 * If query_addr is a direct 1-hop neighbor, resolves directly to its MAC
 * address.
 * If query_addr is a multi-hop destination in LocTE, runs greedy forwarding
 * to select the best next-hop neighbor toward the destination.
 *
 * Return: 0 if linklayer address resolved, negative errno if broadcast needed.
 */
int gn_query_ll_nexthop(struct gn_iface *gnif, gn_address_t query_addr,
			u8 *ll_address)
{
	bool is_neighbor = false;
	struct gn_lpv target_pv;
	struct loc_te *entry;
	bool found = false;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, query_addr) {
		if (entry->addr != query_addr)
			continue;
		if (!GN_TST_VALID(entry->tst_addr))
			break;
		if (entry->is_neighbour &&
		    !is_zero_ether_addr(entry->ll_address)) {
			ether_addr_copy(ll_address, entry->ll_address);
			is_neighbor = true;
		} else {
			target_pv = entry->pv;
		}
		found = true;
		break;
	}
	spin_unlock_bh(&gn_loc_t_lock);

	if (!found)
		return -ENOENT;
	if (is_neighbor)
		return 0;

	return (greedy_forward(gnif, ll_address, &target_pv) ==
		GN_FORWARD_NEXT_HOP) ?
		       0 :
		       -EHOSTUNREACH;
}

/**
 * gn_fill_depv - Populate Destination Position Vector (DEPV) from Location Table
 * @depv: Pointer to gn_spv struct to populate
 * @dest_addr: GeoNetworking address of the destination
 *
 * Return: 0 if valid destination position found in LocTE,
 * negative error code otherwise.
 */
int gn_fill_depv(struct gn_spv *depv, gn_address_t dest_addr)
{
	struct loc_te *entry;
	int rc = -ENOENT;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_possible(gn_loc_t, entry, hnode, dest_addr) {
		if (entry->addr != dest_addr)
			continue;
		if (GN_TST_VALID(entry->tst_addr)) {
			depv->addr = entry->pv.addr;
			depv->tst = entry->pv.tst;
			depv->lat = entry->pv.lat;
			depv->lon = entry->pv.lon;
			rc = 0;
		}
		break;
	}
	spin_unlock_bh(&gn_loc_t_lock);

	return rc;
}

void gn_routing_exit(void)
{
	struct hlist_node *tmp;
	struct loc_te *entry;
	int bucket;

	spin_lock_bh(&gn_loc_t_lock);
	hash_for_each_safe(gn_loc_t, bucket, tmp, entry, hnode) {
		skb_queue_purge(&entry->lsb);
		hash_del(&entry->hnode);
		kfree(entry);
	}
	spin_unlock_bh(&gn_loc_t_lock);
}
