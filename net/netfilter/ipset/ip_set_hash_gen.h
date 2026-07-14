/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2013 Jozsef Kadlecsik <kadlec@netfilter.org> */

#ifndef _IP_SET_HASH_GEN_H
#define _IP_SET_HASH_GEN_H

#include <linux/rcupdate.h>
#include <linux/jhash.h>
#include <linux/types.h>
#include <linux/rhashtable.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/ipset/ip_set.h>

#define ipset_dereference_nfnl(p)	\
	rcu_dereference_protected(p,	\
		lockdep_nfnl_is_held(NFNL_SUBSYS_IPSET))

struct htable_gc {
	struct delayed_work dwork;
	struct ip_set *set;	/* Set the gc belongs to */
};

#ifndef IPSET_NET_COUNT
#define IPSET_NET_COUNT		1
#endif

/* Book-keeping of the prefixes added to the set */
struct net_prefix {
	u8 cidr;			/* the cidr value */
	u32 count;			/* number of elements of this cidr */
};

struct net_prefixes {
	struct rcu_head rcu;
	u8 len;
	struct net_prefix nets[]
		__aligned(__alignof__(u64));
};

#ifdef IP_SET_HASH_WITH_NETS
#if IPSET_NET_COUNT > 1
#define __CIDR(cidr, i)		(cidr[i])
#else
#define __CIDR(cidr, i)		(cidr)
#endif
#ifdef IP_SET_HASH_WITH_NETS_PACKED
/* When cidr is packed with nomatch, cidr - 1 is stored in the data entry */
#define DCIDR_PUT(cidr)		((cidr) - 1)
#define DCIDR_GET(cidr, i)	(__CIDR(cidr, i) + 1)
#else
#define DCIDR_PUT(cidr)		(cidr)
#define DCIDR_GET(cidr, i)	__CIDR(cidr, i)
#endif

#define INIT_CIDR(n, host_mask)	\
	DCIDR_PUT((n)->len ? (n)->nets[0].cidr : host_mask)

#endif /* IP_SET_HASH_WITH_NETS */

#define SET_ELEM_EXPIRED(set, d)	\
	(SET_WITH_TIMEOUT(set) &&	\
	 ip_set_timeout_expired(ext_timeout(d, set)))

#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
static const union nf_inet_addr onesmask = {
	.all[0] = 0xffffffff,
	.all[1] = 0xffffffff,
	.all[2] = 0xffffffff,
	.all[3] = 0xffffffff
};

static const union nf_inet_addr zeromask = {};
#endif

#endif /* _IP_SET_HASH_GEN_H */

#ifndef MTYPE
#error "MTYPE is not defined!"
#endif

#ifndef HTYPE
#error "HTYPE is not defined!"
#endif

#ifndef HOST_MASK
#error "HOST_MASK is not defined!"
#endif

/* Family dependent templates */

#undef mtype_data_equal
#undef mtype_do_data_match
#undef mtype_data_set_flags
#undef mtype_data_reset_elem
#undef mtype_data_reset_flags
#undef mtype_data_netmask
#undef mtype_data_list
#undef mtype_data_next
#undef mtype_elem

#undef mtype_rht_elem
#undef mtype_rht_hashfn
#undef mtype_rht_obj_hashfn
#undef mtype_rht_cmpfn
#undef mtype_rht_params

#undef mtype_add_cidr
#undef mtype_del_cidr
#undef mtype_del_cidr_all
#undef mtype_flush_elem
#undef mtype_flush
#undef mtype_destroy
#undef mtype_same_set
#undef mtype_kadt
#undef mtype_uadt
#undef mtype_bucket_size
#undef mtype_hash_size

#undef mtype_add
#undef mtype_del
#undef mtype_test_cidrs
#undef mtype_test
#undef mtype_uref
#undef mtype_ext_size
#undef mtype_head
#undef mtype_list
#undef mtype_gc
#undef mtype_gc_init
#undef mtype_cancel_gc
#undef mtype_variant
#undef mtype_data_match

#undef htype
#undef HKEY_DATALEN

#define mtype_data_equal	IPSET_TOKEN(MTYPE, _data_equal)
#ifdef IP_SET_HASH_WITH_NETS
#define mtype_do_data_match	IPSET_TOKEN(MTYPE, _do_data_match)
#else
#define mtype_do_data_match(d)	1
#endif
#define mtype_data_set_flags	IPSET_TOKEN(MTYPE, _data_set_flags)
#define mtype_data_reset_elem	IPSET_TOKEN(MTYPE, _data_reset_elem)
#define mtype_data_reset_flags	IPSET_TOKEN(MTYPE, _data_reset_flags)
#define mtype_data_netmask	IPSET_TOKEN(MTYPE, _data_netmask)
#define mtype_data_list		IPSET_TOKEN(MTYPE, _data_list)
#define mtype_data_next		IPSET_TOKEN(MTYPE, _data_next)
#define mtype_elem		IPSET_TOKEN(MTYPE, _elem)

#define mtype_rht_elem		IPSET_TOKEN(MTYPE, _rht_elem)
#define mtype_rht_hashfn	IPSET_TOKEN(MTYPE, _rht_hashfn)
#define mtype_rht_obj_hashfn	IPSET_TOKEN(MTYPE, _rht_obj_hashfn)
#define mtype_rht_cmpfn		IPSET_TOKEN(MTYPE, _rht_cmpfn)
#define mtype_rht_params	IPSET_TOKEN(MTYPE, _rht_params)

#define mtype_add_cidr		IPSET_TOKEN(MTYPE, _add_cidr)
#define mtype_del_cidr		IPSET_TOKEN(MTYPE, _del_cidr)
#define mtype_del_cidr_all	IPSET_TOKEN(MTYPE, _del_cidr_all)
#define mtype_flush_elem	IPSET_TOKEN(MTYPE, _flush_elem)
#define mtype_flush		IPSET_TOKEN(MTYPE, _flush)
#define mtype_destroy		IPSET_TOKEN(MTYPE, _destroy)
#define mtype_same_set		IPSET_TOKEN(MTYPE, _same_set)
#define mtype_kadt		IPSET_TOKEN(MTYPE, _kadt)
#define mtype_uadt		IPSET_TOKEN(MTYPE, _uadt)
#define mtype_bucket_size	IPSET_TOKEN(MTYPE, _bucket_size)
#define mtype_hash_size		IPSET_TOKEN(MTYPE, _hash_size)

#define mtype_add		IPSET_TOKEN(MTYPE, _add)
#define mtype_del		IPSET_TOKEN(MTYPE, _del)
#define mtype_test_cidrs	IPSET_TOKEN(MTYPE, _test_cidrs)
#define mtype_test		IPSET_TOKEN(MTYPE, _test)
#define mtype_uref		IPSET_TOKEN(MTYPE, _uref)
#define mtype_ext_size		IPSET_TOKEN(MTYPE, _ext_size)
#define mtype_head		IPSET_TOKEN(MTYPE, _head)
#define mtype_list		IPSET_TOKEN(MTYPE, _list)
#define mtype_gc		IPSET_TOKEN(MTYPE, _gc)
#define mtype_gc_init		IPSET_TOKEN(MTYPE, _gc_init)
#define mtype_cancel_gc		IPSET_TOKEN(MTYPE, _cancel_gc)
#define mtype_variant		IPSET_TOKEN(MTYPE, _variant)
#define mtype_data_match	IPSET_TOKEN(MTYPE, _data_match)

#ifndef HKEY_DATALEN
#define HKEY_DATALEN		sizeof(struct mtype_elem)
#endif

#define htype			MTYPE

/* Per-element rhashtable object.  Extensions follow the elem field inline;
 * allocate as offsetof(struct mtype_rht_elem, elem) + set->dsize bytes.
 */
struct mtype_rht_elem {
	struct rhash_head node;
	struct rcu_head rcu;		/* deferred free after removal */
	struct mtype_elem elem;		/* element data; extensions follow */
};

/* jhash of the lookup key */
static u32 mtype_rht_hashfn(const void *data, u32 len, u32 seed)
{
	BUILD_BUG_ON(HKEY_DATALEN % sizeof(u32) != 0);
	return jhash2((const u32 *)data, HKEY_DATALEN / sizeof(u32), seed);
}

/* jhash of an existing element object */
static u32 mtype_rht_obj_hashfn(const void *obj, u32 len, u32 seed)
{
	const struct mtype_rht_elem *e = obj;
#ifdef IP_SET_HASH_WITH_NETS
	/* Reset transient flags (e.g. nomatch) before hashing so that the
	 * object hash always equals the lookup-key hash computed by hashfn.
	 */
	struct mtype_elem tmp;
	u8 flags = 0;

	memcpy(&tmp, &e->elem, HKEY_DATALEN);
	mtype_data_reset_flags(&tmp, &flags);
	return jhash2((const u32 *)&tmp, HKEY_DATALEN / sizeof(u32), seed);
#else
	return jhash2((const u32 *)&e->elem, HKEY_DATALEN / sizeof(u32), seed);
#endif
}

/* 0 = key matches object (equal), non-zero = not equal */
static int mtype_rht_cmpfn(struct rhashtable_compare_arg *arg, const void *obj)
{
	const struct mtype_rht_elem *e = obj;
	u32 multi = 0;

	return !mtype_data_equal(&e->elem,
				 (const struct mtype_elem *)arg->key, &multi);
}

static const struct rhashtable_params mtype_rht_params = {
	.head_offset	= offsetof(struct mtype_rht_elem, node),
	.hashfn		= mtype_rht_hashfn,
	.obj_hashfn	= mtype_rht_obj_hashfn,
	.obj_cmpfn	= mtype_rht_cmpfn,
	.key_len	= HKEY_DATALEN,
	.automatic_shrinking = true,
};

/* The hash set type */
struct htype {
	struct rhashtable ht;	/* the hash table */
	u32 maxelem;		/* max element limit (user-requested) */
	struct net_prefixes __rcu *rnets[IPSET_NET_COUNT]; /* cidr prefixes */
	struct htable_gc gc;	/* gc workqueue */
#ifdef IP_SET_HASH_WITH_MARKMASK
	u32 markmask;		/* markmask value for mark mask to store */
#endif
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	u8 netmask;		/* netmask value for subnets to store */
	union nf_inet_addr bitmask;	/* stores bitmask */
#endif
	/* Because 'next' is IPv4/IPv6 dependent, no elements of this
	 * structure and referred in create() may come after 'next'.
	 */
	struct mtype_elem next; /* temporary storage for uadd */
};

#ifdef IP_SET_HASH_WITH_NETS
/* Network cidr size book keeping when the hash stores different
 * sized networks. cidr == real cidr + 1 to support /0.
 */
static int
mtype_add_cidr(struct ip_set *set, struct htype *h, u8 cidr, u8 n)
{
	struct net_prefixes *nets, *tmp;
	int i, j, found, len = 0, ret = 0;

	spin_lock_bh(&set->lock);
	nets = ipset_dereference_locked(h->rnets[n], set);
	/* Add in increasing prefix order, so larger cidr first */
	for (i = 0, found = -1; i < nets->len; i++) {
		if (nets->nets[i].count)
			len++;
		if (found != -1) {
			continue;
		} else if (nets->nets[i].cidr < cidr) {
			found = i;
		} else if (nets->nets[i].cidr == cidr) {
			nets->nets[i].count++;
			goto unlock;
		}
	}
	len++;
	tmp = kzalloc(sizeof(struct net_prefixes) +
		      len * sizeof(struct net_prefix), GFP_ATOMIC);
	if (!tmp) {
		ret = -ENOMEM;
		goto unlock;
	}

	tmp->len = len;
	for (i = 0, j = 0; i < nets->len; i++) {
		if (!nets->nets[i].count)
			continue;
		if (i == found) {
			tmp->nets[j].cidr = cidr;
			tmp->nets[j++].count = 1;
		}
		tmp->nets[j].cidr = nets->nets[i].cidr;
		tmp->nets[j++].count = nets->nets[i].count;
	}
	if (found == -1) {
		tmp->nets[j].cidr = cidr;
		tmp->nets[j].count = 1;
	}
	rcu_assign_pointer(h->rnets[n], tmp);
	kfree_rcu(nets, rcu);
unlock:
	spin_unlock_bh(&set->lock);
	return ret;
}

static void
mtype_del_cidr(struct ip_set *set, struct htype *h, u8 cidr, u8 n)
{
	struct net_prefixes *nets, *tmp;
	u8 i, j, len = 0;
	int found;

	spin_lock_bh(&set->lock);
	nets = ipset_dereference_locked(h->rnets[n], set);
	for (i = 0, found = -1; i < nets->len; i++) {
		if (nets->nets[i].count)
			len++;
		if (nets->nets[i].cidr == cidr)
			found = i;
	}
	if (unlikely(found == -1))
		goto unlock;

	nets->nets[found].count--;
	if (nets->nets[found].count)
		goto unlock;
	len--;
	tmp = kzalloc(sizeof(struct net_prefixes) +
		      len * sizeof(struct net_prefix), GFP_ATOMIC);
	if (!tmp)
		/* Leave a hole */
		goto unlock;

	tmp->len = len;
	for (i = 0, j = 0; i < nets->len; i++) {
		if (!nets->nets[i].count || i == found)
			continue;
		tmp->nets[j].cidr = nets->nets[i].cidr;
		tmp->nets[j++].count = nets->nets[i].count;
	}
	rcu_assign_pointer(h->rnets[n], tmp);
	kfree_rcu(nets, rcu);
unlock:
	spin_unlock_bh(&set->lock);
}
#endif

static void
mtype_del_cidr_all(struct ip_set *set, struct htype *h, const struct mtype_elem *data)
{
#ifdef IP_SET_HASH_WITH_NETS
	int k;

	for (k = 0; k < IPSET_NET_COUNT; k++)
		mtype_del_cidr(set, h, DCIDR_GET(data->cidr, k), k);
#endif
}

/* Free one element: called by rhashtable_free_and_destroy */
static void
mtype_flush_elem(void *ptr, void *arg)
{
	struct ip_set *set = arg;
	struct mtype_rht_elem *e = ptr;

	ip_set_ext_destroy(set, &e->elem);
	kfree_rcu(e, rcu);
}

/* Flush a hash type of set: destroy all elements */
static void
mtype_flush(struct ip_set *set)
{
	struct htype *h = set->data;
	struct rhashtable_iter hti;
	struct mtype_rht_elem *e;

	rhashtable_walk_enter(&h->ht, &hti);
	rhashtable_walk_start(&hti);

	while ((e = rhashtable_walk_next(&hti))) {
		if (IS_ERR(e)) {
			if (PTR_ERR(e) == -EAGAIN)
				continue;
			break;
		}

		if (rhashtable_remove_fast(&h->ht, &e->node, mtype_rht_params))
			continue; /* Concurrent delete? skip */

		mtype_del_cidr_all(set, h, &e->elem);
		ip_set_ext_destroy_slow(set, &e->elem);
		kfree_rcu(e, rcu);
	}
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);
}

/* Destroy a hash type of set */
static void
mtype_destroy(struct ip_set *set)
{
	struct htype *h = set->data;
#ifdef IP_SET_HASH_WITH_NETS
	u32 i;
#endif

	rhashtable_free_and_destroy(&h->ht, mtype_flush_elem, set);

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++)
		kfree(h->rnets[i]);
#endif
	kfree(h);

	set->data = NULL;
}

static bool
mtype_same_set(const struct ip_set *a, const struct ip_set *b)
{
	const struct htype *x = a->data;
	const struct htype *y = b->data;

	return x->maxelem == y->maxelem &&
	       a->timeout == b->timeout &&
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	       nf_inet_addr_cmp(&x->bitmask, &y->bitmask) &&
#endif
#ifdef IP_SET_HASH_WITH_MARKMASK
	       x->markmask == y->markmask &&
#endif
	       a->extensions == b->extensions;
}

static void
mtype_gc(struct work_struct *work)
{
	struct htable_gc *gc;
	struct ip_set *set;
	struct htype *h;
	struct rhashtable_iter hti;
	struct mtype_rht_elem *e;
	unsigned int next_run;

	gc = container_of(work, struct htable_gc, dwork.work);
	set = gc->set;
	h = set->data;

	next_run = IPSET_GC_PERIOD(set->timeout) * HZ;
	if (next_run < HZ)
		next_run = HZ;

	rhashtable_walk_enter(&h->ht, &hti);
	rhashtable_walk_start(&hti);
	while ((e = rhashtable_walk_next(&hti))) {
		if (IS_ERR(e)) {
			if (PTR_ERR(e) == -EAGAIN)
				continue;
			break;
		}
		if (!ip_set_timeout_expired(ext_timeout(&e->elem, set)))
			continue;
		if (rhashtable_remove_fast(&h->ht, &e->node, mtype_rht_params))
			continue; /* Concurrent delete? skip */

		mtype_del_cidr_all(set, h, &e->elem);
		ip_set_ext_destroy_slow(set, &e->elem);
		kfree_rcu(e, rcu);
	}
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);

	queue_delayed_work(system_power_efficient_wq, &gc->dwork, next_run);
}

static void
mtype_gc_init(struct htable_gc *gc)
{
	INIT_DEFERRABLE_WORK(&gc->dwork, mtype_gc);
	queue_delayed_work(system_power_efficient_wq, &gc->dwork, HZ);
}

static void
mtype_cancel_gc(struct ip_set *set)
{
	struct htype *h = set->data;

	if (SET_WITH_TIMEOUT(set))
		disable_delayed_work_sync(&h->gc.dwork);
}

/* Get the current number of elements and per-element memory in the set */
static void
mtype_ext_size(struct ip_set *set, u32 *elements, size_t *ext_size)
{
	const struct htype *h = set->data;

	*elements = atomic_read(&h->ht.nelems);
	*ext_size = *elements *
		    (offsetof(struct mtype_rht_elem, elem) + set->dsize);
}

/* Add an element to a hash and update the internal counters when succeeded,
 * otherwise report the proper error code.
 */
static int
mtype_add(struct ip_set *set, void *value, const struct ip_set_ext *ext,
	  struct ip_set_ext *mext, u32 flags)
{
	struct htype *h = set->data;
	const struct mtype_elem *d = value;
	struct mtype_rht_elem *e, *old;
	bool flag_exist = flags & IPSET_FLAG_EXIST;
	int ret = 0;
#ifdef IP_SET_HASH_WITH_NETS
	int i;
#endif

	/* Check for an existing entry with the same key */
	rcu_read_lock();
	old = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (old) {
		if (!SET_ELEM_EXPIRED(set, &old->elem)) {
			if (!flag_exist) {
				rcu_read_unlock();
				return -IPSET_ERR_EXIST;
			}
			/* flag_exist: overwrite extensions in-place.
			 * Hold set->lock to serialize ext_size accounting in
			 * ip_set_init_comment against concurrent kernel-side adds.
			 * rcu_read_lock() must remain held to keep old alive.
			 */
			spin_lock_bh(&set->lock);
#ifdef IP_SET_HASH_WITH_NETS
			mtype_data_set_flags(&old->elem, flags);
#endif
			if (SET_WITH_COUNTER(set))
				ip_set_init_counter(ext_counter(&old->elem, set),
						    ext);
			if (SET_WITH_COMMENT(set))
				ip_set_init_comment(set,
						    ext_comment(&old->elem, set),
						    ext);
			if (SET_WITH_SKBINFO(set))
				ip_set_init_skbinfo(ext_skbinfo(&old->elem, set),
						    ext);
			if (SET_WITH_TIMEOUT(set))
				ip_set_timeout_set(ext_timeout(&old->elem, set),
						   ext->timeout);
			spin_unlock_bh(&set->lock);
			rcu_read_unlock();
			return 0;
		}
		/* Expired entry: remove it to make room */
		if (rhashtable_remove_fast(&h->ht, &old->node,
					   mtype_rht_params) == 0) {
			mtype_del_cidr_all(set, h, &old->elem);
			ip_set_ext_destroy_slow(set, &old->elem);
			kfree_rcu(old, rcu);
		}
	}
	rcu_read_unlock();

	if (atomic_read(&h->ht.nelems) >= h->maxelem) {
		if (net_ratelimit())
			pr_warn("Set %s is full, maxelem %u reached\n",
				set->name, h->maxelem);
		mtype_data_next(&h->next, d);
		return -IPSET_ERR_HASH_FULL;
	}

	e = kzalloc(offsetof(struct mtype_rht_elem, elem) + set->dsize,
		    GFP_ATOMIC);
	if (!e)
		return -ENOMEM;

	memcpy(&e->elem, d, sizeof(struct mtype_elem));

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++)
		mtype_add_cidr(set, h, DCIDR_GET(d->cidr, i), i);

	mtype_data_set_flags(&e->elem, flags);
#endif
	if (SET_WITH_COUNTER(set))
		ip_set_init_counter(ext_counter(&e->elem, set), ext);
	if (SET_WITH_COMMENT(set))
		ip_set_init_comment_slow(set, ext_comment(&e->elem, set), ext);
	if (SET_WITH_SKBINFO(set))
		ip_set_init_skbinfo(ext_skbinfo(&e->elem, set), ext);
	/* Must come last for the case when timed out entry is reused */
	if (SET_WITH_TIMEOUT(set))
		ip_set_timeout_set(ext_timeout(&e->elem, set), ext->timeout);

	ret = rhashtable_insert_fast(&h->ht, &e->node, mtype_rht_params);
	if (ret) {
		mtype_del_cidr_all(set, h, d);
		ip_set_ext_destroy_slow(set, &e->elem);
		kfree(e);
		if (ret == -EEXIST)
			ret = flag_exist ? 0 : -IPSET_ERR_EXIST;
	}
	return ret;
}

/* Delete an element from the hash */
static int
mtype_del(struct ip_set *set, void *value, const struct ip_set_ext *ext,
	  struct ip_set_ext *mext, u32 flags)
{
	struct htype *h = set->data;
	const struct mtype_elem *d = value;
	struct mtype_rht_elem *e;
	int ret = -IPSET_ERR_EXIST;

	rcu_read_lock();
	e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (!e) {
		rcu_read_unlock();
		return -IPSET_ERR_EXIST;
	}
	ret = rhashtable_remove_fast(&h->ht, &e->node, mtype_rht_params);
	rcu_read_unlock();

	if (ret)
		return -IPSET_ERR_EXIST;

	mtype_del_cidr_all(set, h, d);
	ip_set_ext_destroy_slow(set, &e->elem);
	kfree_rcu(e, rcu);
	return 0;
}

static int
mtype_data_match(struct mtype_elem *data, const struct ip_set_ext *ext,
		 struct ip_set_ext *mext, struct ip_set *set, u32 flags)
{
	if (!ip_set_match_extensions(set, ext, mext, flags, data))
		return 0;
	/* nomatch entries return -ENOTEMPTY */
	return mtype_do_data_match(data);
}

#ifdef IP_SET_HASH_WITH_NETS
/* Special test function which takes into account the different network
 * sizes added to the set
 */
static int
mtype_test_cidrs(struct ip_set *set, struct mtype_elem *d,
		 const struct ip_set_ext *ext,
		 struct ip_set_ext *mext, u32 flags)
{
	struct htype *h = set->data;
	struct net_prefixes *nets0;
	struct mtype_rht_elem *e;
#if IPSET_NET_COUNT == 2
	struct net_prefixes *nets1;
	struct mtype_elem orig = *d;
	int ret, j, k;
#else
	int ret, j;
#endif
	u32 multi = 0;

	pr_debug("test by nets\n");
	nets0 = rcu_dereference(h->rnets[0]);
#if IPSET_NET_COUNT == 2
	nets1 = rcu_dereference(h->rnets[1]);
#endif
	for (j = 0; j < nets0->len && !multi; j++) {
		if (!nets0->nets[j].count)
			continue;
#if IPSET_NET_COUNT == 2
		mtype_data_reset_elem(d, &orig);
		mtype_data_netmask(d, nets0->nets[j].cidr, false);
		for (k = 0; k < nets1->len && !multi; k++) {
			if (!nets1->nets[k].count)
				continue;
			mtype_data_netmask(d, nets1->nets[k].cidr, true);
#else
		mtype_data_netmask(d, nets0->nets[j].cidr);
#endif
		e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
		if (e) {
			ret = mtype_data_match(&e->elem, ext, mext, set, flags);
			if (ret != 0)
				return ret;
#ifdef IP_SET_HASH_WITH_MULTI
			/* No match, reset multiple match flag */
			multi = 0;
#endif
		}
#if IPSET_NET_COUNT == 2
		}
#endif
	}
	return 0;
}
#endif

/* Test whether the element is added to the set */
static int
mtype_test(struct ip_set *set, void *value, const struct ip_set_ext *ext,
	   struct ip_set_ext *mext, u32 flags)
{
	struct htype *h = set->data;
	struct mtype_elem *d = value;
	struct mtype_rht_elem *e;
	int ret = 0;
#ifdef IP_SET_HASH_WITH_NETS
	int i;
#endif

	rcu_read_lock();
#ifdef IP_SET_HASH_WITH_NETS
	/* If we test an IP address and not a network address,
	 * try all possible network sizes
	 */
	for (i = 0; i < IPSET_NET_COUNT; i++)
		if (DCIDR_GET(d->cidr, i) != HOST_MASK)
			break;
	if (i == IPSET_NET_COUNT) {
		ret = mtype_test_cidrs(set, d, ext, mext, flags);
		goto out;
	}
#endif

	e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (!e || SET_ELEM_EXPIRED(set, &e->elem)) {
		ret = 0;
		goto out;
	}

	ret = mtype_data_match(&e->elem, ext, mext, set, flags);
out:
	rcu_read_unlock();
	return ret;
}

static u32 mtype_hash_size(const struct htype *h)
{
	const struct bucket_table *tbl;
	u32 size = 0;

	rcu_read_lock();
	tbl = rcu_dereference(h->ht.tbl);
	if (tbl)
		size = tbl->size;
	rcu_read_unlock();

	return size;
}

static u32 mtype_bucket_size(const struct htype *h)
{
	unsigned int nelems = atomic_read(&h->ht.nelems);
	u32 size = mtype_hash_size(h);

	return nelems / size;
}

/* Reply a HEADER request: fill out the header part of the set */
static int
mtype_head(struct ip_set *set, struct sk_buff *skb)
{
	struct htype *h = set->data;
	struct nlattr *nested;
	size_t memsize;
	u32 elements = 0;
	size_t ext_size = 0;

	mtype_ext_size(set, &elements, &ext_size);
	memsize = sizeof(*h) + ext_size + set->ext_size;

	nested = nla_nest_start(skb, IPSET_ATTR_DATA);
	if (!nested)
		goto nla_put_failure;

	if (nla_put_net32(skb, IPSET_ATTR_HASHSIZE, htonl(mtype_hash_size(h))))
		goto nla_put_failure;
	if (nla_put_net32(skb, IPSET_ATTR_MAXELEM, htonl(h->maxelem)))
		goto nla_put_failure;
#ifdef IP_SET_HASH_WITH_BITMASK
	/* if netmask is set to anything other than HOST_MASK we know that the user supplied netmask
	 * and not bitmask. These two are mutually exclusive. */
	if (h->netmask == HOST_MASK && !nf_inet_addr_cmp(&onesmask, &h->bitmask)) {
		if (set->family == NFPROTO_IPV4) {
			if (nla_put_ipaddr4(skb, IPSET_ATTR_BITMASK, h->bitmask.ip))
				goto nla_put_failure;
		} else if (set->family == NFPROTO_IPV6) {
			if (nla_put_ipaddr6(skb, IPSET_ATTR_BITMASK, &h->bitmask.in6))
				goto nla_put_failure;
		}
	}
#endif
#ifdef IP_SET_HASH_WITH_NETMASK
	if (h->netmask != HOST_MASK && nla_put_u8(skb, IPSET_ATTR_NETMASK, h->netmask))
		goto nla_put_failure;
#endif
#ifdef IP_SET_HASH_WITH_MARKMASK
	if (nla_put_u32(skb, IPSET_ATTR_MARKMASK, h->markmask))
		goto nla_put_failure;
#endif
	if (set->flags & IPSET_CREATE_FLAG_BUCKETSIZE) {
		if (nla_put_u8(skb, IPSET_ATTR_BUCKETSIZE, mtype_bucket_size(h)))
			goto nla_put_failure;
		if (nla_put_u32(skb, IPSET_ATTR_INITVAL, 0))
			goto nla_put_failure;
	}
	if (nla_put_net32(skb, IPSET_ATTR_REFERENCES, htonl(set->ref)) ||
	    nla_put_net32(skb, IPSET_ATTR_MEMSIZE, htonl(memsize)) ||
	    nla_put_net32(skb, IPSET_ATTR_ELEMENTS, htonl(elements)))
		goto nla_put_failure;
	if (unlikely(ip_set_put_flags(skb, set)))
		goto nla_put_failure;
	nla_nest_end(skb, nested);

	return 0;
nla_put_failure:
	return -EMSGSIZE;
}

/* Manage the rhashtable_iter lifetime for dump operations */
static void
mtype_uref(struct ip_set *set, struct netlink_callback *cb, bool start)
{
	struct htype *h = set->data;
	struct rhashtable_iter *hti;

	if (start) {
		hti = kmalloc(sizeof(*hti), GFP_ATOMIC);
		if (hti)
			rhashtable_walk_enter(&h->ht, hti);
		cb->args[IPSET_CB_PRIVATE] = (unsigned long)hti;
	} else {
		hti = (struct rhashtable_iter *)cb->args[IPSET_CB_PRIVATE];
		if (hti) {
			rhashtable_walk_exit(hti);
			kfree(hti);
		}
		cb->args[IPSET_CB_PRIVATE] = 0;
	}
}

/* Reply a LIST/SAVE request: dump the elements of the specified set */
static int
mtype_list(const struct ip_set *set,
	   struct sk_buff *skb, struct netlink_callback *cb)
{
	struct rhashtable_iter *hti =
		(struct rhashtable_iter *)cb->args[IPSET_CB_PRIVATE];
	struct mtype_rht_elem *e, *peeked;
	struct nlattr *atd, *nested;
	void *incomplete;
	u32 emitted = 0;
	int ret = 0;

	if (!hti)
		return -EMSGSIZE;

	atd = nla_nest_start(skb, IPSET_ATTR_ADT);
	if (!atd)
		return -EMSGSIZE;

	rhashtable_walk_start(hti);
	while ((e = rhashtable_walk_peek(hti))) {
		if (IS_ERR(e)) {
			if (PTR_ERR(e) == -EAGAIN)
				continue;
			ret = PTR_ERR(e);
			break;
		}
		peeked = e;
next_dump:
		if (SET_ELEM_EXPIRED(set, &e->elem))
			goto next_entry;

		incomplete = skb_tail_pointer(skb);
		nested = nla_nest_start(skb, IPSET_ATTR_DATA);
		if (!nested) {
			nlmsg_trim(skb, incomplete);
			goto paused;
		}
		if (mtype_data_list(skb, &e->elem) ||
		    ip_set_put_extensions(skb, set, &e->elem, true)) {
			nla_nest_cancel(skb, nested);
			nlmsg_trim(skb, incomplete);
			goto paused;
		}
		nla_nest_end(skb, nested);
		emitted++;
next_entry:
		e = rhashtable_walk_next(hti);
		if (IS_ERR(e)) {
			ret = PTR_ERR(e);
			if (ret != -EAGAIN)
				break;
			ret = 0;
		} else if (peeked && e != peeked) {
			peeked = NULL;
			if (e)
				goto next_dump;
		}
	}
	/* Walk exhausted: listing done */
	nla_nest_end(skb, atd);
	rhashtable_walk_stop(hti);
	cb->args[IPSET_CB_ARG0] = 0;
	return ret;

paused:
	if (emitted == 0) {
		nla_nest_cancel(skb, atd);
		rhashtable_walk_stop(hti);
		return -EMSGSIZE;
	}
	cb->args[IPSET_CB_ARG0] = 1;
	nla_nest_end(skb, atd);
	rhashtable_walk_stop(hti);
	return 0;
}

static int
IPSET_TOKEN(MTYPE, _kadt)(struct ip_set *set, const struct sk_buff *skb,
			  const struct xt_action_param *par,
			  enum ipset_adt adt, struct ip_set_adt_opt *opt);

static int
IPSET_TOKEN(MTYPE, _uadt)(struct ip_set *set, struct nlattr *tb[],
			  enum ipset_adt adt, u32 *lineno, u32 flags,
			  bool retried);

static const struct ip_set_type_variant mtype_variant = {
	.kadt	= mtype_kadt,
	.uadt	= mtype_uadt,
	.adt	= {
		[IPSET_ADD] = mtype_add,
		[IPSET_DEL] = mtype_del,
		[IPSET_TEST] = mtype_test,
	},
	.destroy = mtype_destroy,
	.flush	= mtype_flush,
	.head	= mtype_head,
	.list	= mtype_list,
	.uref	= mtype_uref,
	.resize	= NULL,
	.same_set = mtype_same_set,
	.cancel_gc = mtype_cancel_gc,
	.region_lock = true,
};

#ifdef IP_SET_EMIT_CREATE
static int
IPSET_TOKEN(HTYPE, _create)(struct net *net, struct ip_set *set,
			    struct nlattr *tb[], u32 flags)
{
	struct rhashtable_params params;
	u32 hashsize = IPSET_DEFAULT_HASHSIZE, maxelem = IPSET_DEFAULT_MAXELEM;
#ifdef IP_SET_HASH_WITH_MARKMASK
	u32 markmask;
#endif
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	int ret __attribute__((unused)) = 0;
	u8 netmask = set->family == NFPROTO_IPV4 ? 32 : 128;
	union nf_inet_addr bitmask = onesmask;
#endif
#ifdef IP_SET_HASH_WITH_NETS
	struct net_prefixes *nets;
	int i;
#endif
	size_t hsize;
	struct htype *h;
	int err;

	pr_debug("Create set %s with family %s\n",
		 set->name, set->family == NFPROTO_IPV4 ? "inet" : "inet6");

#ifdef IP_SET_PROTO_UNDEF
	if (set->family != NFPROTO_UNSPEC)
		return -IPSET_ERR_INVALID_FAMILY;
#else
	if (!(set->family == NFPROTO_IPV4 || set->family == NFPROTO_IPV6))
		return -IPSET_ERR_INVALID_FAMILY;
#endif

	if (unlikely(!ip_set_optattr_netorder(tb, IPSET_ATTR_HASHSIZE) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_MAXELEM) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_TIMEOUT) ||
		     !ip_set_optattr_netorder(tb, IPSET_ATTR_CADT_FLAGS)))
		return -IPSET_ERR_PROTOCOL;

#ifdef IP_SET_HASH_WITH_MARKMASK
	/* Separated condition in order to avoid directive in argument list */
	if (unlikely(!ip_set_optattr_netorder(tb, IPSET_ATTR_MARKMASK)))
		return -IPSET_ERR_PROTOCOL;

	markmask = 0xffffffff;
	if (tb[IPSET_ATTR_MARKMASK]) {
		markmask = ntohl(nla_get_be32(tb[IPSET_ATTR_MARKMASK]));
		if (markmask == 0)
			return -IPSET_ERR_INVALID_MARKMASK;
	}
#endif

#ifdef IP_SET_HASH_WITH_NETMASK
	if (tb[IPSET_ATTR_NETMASK]) {
		netmask = nla_get_u8(tb[IPSET_ATTR_NETMASK]);

		if ((set->family == NFPROTO_IPV4 && netmask > 32) ||
		    (set->family == NFPROTO_IPV6 && netmask > 128) ||
		    netmask == 0)
			return -IPSET_ERR_INVALID_NETMASK;

		/* we convert netmask to bitmask and store it */
		if (set->family == NFPROTO_IPV4)
			bitmask.ip = ip_set_netmask(netmask);
		else
			ip6_netmask(&bitmask, netmask);
	}
#endif

#ifdef IP_SET_HASH_WITH_BITMASK
	if (tb[IPSET_ATTR_BITMASK]) {
		/* bitmask and netmask do the same thing, allow only one of these options */
		if (tb[IPSET_ATTR_NETMASK])
			return -IPSET_ERR_BITMASK_NETMASK_EXCL;

		if (set->family == NFPROTO_IPV4) {
			ret = ip_set_get_ipaddr4(tb[IPSET_ATTR_BITMASK], &bitmask.ip);
			if (ret || !bitmask.ip)
				return -IPSET_ERR_INVALID_NETMASK;
		} else if (set->family == NFPROTO_IPV6) {
			ret = ip_set_get_ipaddr6(tb[IPSET_ATTR_BITMASK], &bitmask);
			if (ret || ipv6_addr_any(&bitmask.in6))
				return -IPSET_ERR_INVALID_NETMASK;
		}

		if (nf_inet_addr_cmp(&bitmask, &zeromask))
			return -IPSET_ERR_INVALID_NETMASK;
	}
#endif

	if (tb[IPSET_ATTR_HASHSIZE]) {
		hashsize = ip_set_get_h32(tb[IPSET_ATTR_HASHSIZE]);
		if (hashsize < IPSET_MIMINAL_HASHSIZE)
			hashsize = IPSET_MIMINAL_HASHSIZE;
	}

	if (tb[IPSET_ATTR_MAXELEM])
		maxelem = ip_set_get_h32(tb[IPSET_ATTR_MAXELEM]);

#ifdef IP_SET_PROTO_UNDEF
	hsize = sizeof(struct htype);
	params = mtype_rht_params;
#else
	if (set->family == NFPROTO_IPV6) {
		hsize = sizeof(struct IPSET_TOKEN(HTYPE, 6));
		params = IPSET_TOKEN(HTYPE, 6_rht_params);
	} else {
		hsize = sizeof(struct IPSET_TOKEN(HTYPE, 4));
		params = IPSET_TOKEN(HTYPE, 4_rht_params);
	}
#endif
	h = kzalloc(hsize, GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	/* Initialize rhashtable with the user-requested size as hint */
	params.nelem_hint = hashsize;
	/* maxsize: maximum bucket table size to expand to */
	params.max_size = maxelem;

	err = rhashtable_init(&h->ht, &params);
	if (err)
		goto free_h;

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++) {
		nets = kzalloc(sizeof(struct net_prefixes), GFP_KERNEL);
		if (!nets) {
			while (i > 0)
				kfree(h->rnets[--i]);
			goto free_rht;
		}
		RCU_INIT_POINTER(h->rnets[i], nets);
	}
#endif
	h->maxelem = maxelem;
	h->gc.set = set;
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	h->bitmask = bitmask;
	h->netmask = netmask;
#endif
#ifdef IP_SET_HASH_WITH_MARKMASK
	h->markmask = markmask;
#endif
	set->data = h;

#ifndef IP_SET_PROTO_UNDEF
	if (set->family == NFPROTO_IPV4) {
#endif
		set->variant = &IPSET_TOKEN(HTYPE, 4_variant);
		set->dsize = ip_set_elem_len(set, tb,
			sizeof(struct IPSET_TOKEN(HTYPE, 4_elem)),
			__alignof__(struct IPSET_TOKEN(HTYPE, 4_elem)));
#ifndef IP_SET_PROTO_UNDEF
	} else {
		set->variant = &IPSET_TOKEN(HTYPE, 6_variant);
		set->dsize = ip_set_elem_len(set, tb,
			sizeof(struct IPSET_TOKEN(HTYPE, 6_elem)),
			__alignof__(struct IPSET_TOKEN(HTYPE, 6_elem)));
	}
#endif
	set->timeout = IPSET_NO_TIMEOUT;
	if (tb[IPSET_ATTR_TIMEOUT]) {
		set->timeout = ip_set_timeout_uget(tb[IPSET_ATTR_TIMEOUT]);
#ifndef IP_SET_PROTO_UNDEF
		if (set->family == NFPROTO_IPV4)
#endif
			IPSET_TOKEN(HTYPE, 4_gc_init)(&h->gc);
#ifndef IP_SET_PROTO_UNDEF
		else
			IPSET_TOKEN(HTYPE, 6_gc_init)(&h->gc);
#endif
	}
	pr_debug("create %s hashsize %u maxelem %u\n",
		 set->name, mtype_hash_size(h), h->maxelem);

	return 0;

#ifdef IP_SET_HASH_WITH_NETS
free_rht:
	rhashtable_free_and_destroy(&h->ht, mtype_flush_elem, set);
#endif
free_h:
	kfree(h);
	return err ? err : -ENOMEM;
}
#endif /* IP_SET_EMIT_CREATE */

#undef HKEY_DATALEN
