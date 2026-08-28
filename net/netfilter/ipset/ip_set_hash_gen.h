/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2013 Jozsef Kadlecsik <kadlec@netfilter.org> */

#ifndef _IP_SET_HASH_GEN_H
#define _IP_SET_HASH_GEN_H

#include <linux/rcupdate.h>
#include <linux/jhash.h>
#include <linux/types.h>
#include <linux/rhashtable.h>
#include <linux/seqlock.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/ipset/ip_set.h>

#define __ipset_dereference(p)		\
	rcu_dereference_protected(p, 1)
#define ipset_dereference_nfnl(p)	\
	rcu_dereference_protected(p,	\
		lockdep_nfnl_is_held(NFNL_SUBSYS_IPSET))
#define ipset_dereference_bh_nfnl(p)	\
	rcu_dereference_bh_check(p, 	\
		lockdep_nfnl_is_held(NFNL_SUBSYS_IPSET))

/* Kept for backward compatibility */
#define AHASH_INIT_SIZE			2
#define AHASH_MAX_SIZE			(6 * AHASH_INIT_SIZE)

#ifdef IP_SET_HASH_WITH_MULTI
#define RHL_MAX_CHAINLEN	64
#endif

struct htable_gc {
	struct delayed_work dwork;
	struct ip_set *set;	/* Set the gc belongs to */
};

#ifndef IPSET_NET_COUNT
#define IPSET_NET_COUNT		1
#endif

/**
 * struct net_prefix - Representation of a network prefix.
 * @cidr: The CIDR prefix length.
 * @count: Number of occurrences.
 */
struct net_prefix {
	u32 cidr:8;
	u32 count:24;
};

#define CIDR_MAX_COUNT ((1 << 24) - 1)

/**
 * struct net_prefixes - A collection of network prefixes.
 * @rcu: RCU head
 * @seq: Sequence counter guarding in-place reordering of @nets
 * @len: Number of entries in the array.
 * @nets: Array of net_prefix structures (sorted by CIDR descending).
 *
 * @nets entries are updated in place under @set's lock. A single entry's
 * cidr/count pair is always updated atomically via READ_ONCE()/WRITE_ONCE(),
 * but removing an entry also shifts every following entry down by one slot.
 * Lockless readers that scan the whole array (i.e. more than a single
 * indexed slot) must use @seq to detect and retry across such a shift.
 */
struct net_prefixes {
	struct rcu_head rcu;
	seqcount_spinlock_t seq;
	u8 len;
	struct net_prefix nets[] __counted_by(len);
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

#define INIT_CIDR(n, host_mask) ({				\
	const struct net_prefixes *__n = rcu_dereference(n);	\
	struct net_prefix __p =					\
		__n->len ? READ_ONCE(__n->nets[0])		\
			 : (struct net_prefix){};		\
	DCIDR_PUT(__p.count ? __p.cidr : host_mask);		\
})

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

#undef ahash_data
#undef mtype_key_equal
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

#undef mtype_add
#undef mtype_do_set_exts
#undef mtype_del
#undef mtype_test_cidrs
#undef mtype_test
#undef mtype_uref
#undef mtype_rht_size
#undef mtype_head
#undef mtype_list
#undef mtype_gc_do
#undef mtype_gc
#undef mtype_gc_init
#undef mtype_cancel_gc
#undef mtype_variant
#undef mtype_data_match

#undef htype

#ifdef IP_SET_HASH_WITH_MULTI
#define mtype_key_equal	IPSET_TOKEN(MTYPE, _key_equal)
#endif
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

#define mtype_add		IPSET_TOKEN(MTYPE, _add)
#define mtype_del		IPSET_TOKEN(MTYPE, _del)
#define mtype_test_cidrs	IPSET_TOKEN(MTYPE, _test_cidrs)
#define mtype_test		IPSET_TOKEN(MTYPE, _test)
#define mtype_uref		IPSET_TOKEN(MTYPE, _uref)
#define mtype_rht_size		IPSET_TOKEN(MTYPE, _rht_size)
#define mtype_head		IPSET_TOKEN(MTYPE, _head)
#define mtype_list		IPSET_TOKEN(MTYPE, _list)
#define mtype_gc_do		IPSET_TOKEN(MTYPE, _gc_do)
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
#ifdef IP_SET_HASH_WITH_MULTI
	struct rhlist_head node;
#else
	struct rhash_head node;
#endif
	struct rcu_head rcu;		/* deferred free after removal */

	/* element data; must be last. extensions follow. */
	struct mtype_elem elem __aligned(__alignof__(u64));
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
#ifdef IP_SET_HASH_WITH_MULTI
	return !mtype_key_equal(&e->elem, (const struct mtype_elem *)arg->key);
#else
	u32 multi = 0;

	return !mtype_data_equal(&e->elem,
				 (const struct mtype_elem *)arg->key, &multi);
#endif
}

static const struct rhashtable_params mtype_rht_params = {
	.head_offset	= offsetof(struct mtype_rht_elem, node),
	.key_offset	= offsetof(struct mtype_rht_elem, elem),
	.hashfn		= mtype_rht_hashfn,
	.obj_hashfn	= mtype_rht_obj_hashfn,
	.obj_cmpfn	= mtype_rht_cmpfn,
	.key_len	= HKEY_DATALEN,
};

/* The hash set type */
struct htype {
#ifdef IP_SET_HASH_WITH_MULTI
	struct rhltable rhlt;	/* the hashlist table */
#else
	struct rhashtable ht;	/* the hash table */
#endif
	struct net_prefixes __rcu *rnets[IPSET_NET_COUNT]; /* cidr prefixes */
	struct htable_gc gc;	/* gc workqueue */
	u32 maxelem;		/* max elements in the hash */
	u32 initval;		/* kept for backward compatibility */
#ifdef IP_SET_HASH_WITH_MARKMASK
	u32 markmask;		/* markmask value for mark mask to store */
#endif
	u8 htable_bits;		/* kept for backward compatibility */
	u8 bucketsize;		/* kept for backward compatibility */
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	u8 netmask;		/* netmask value for subnets to store */
	union nf_inet_addr bitmask;	/* stores bitmask */
#endif
	/* Because 'next' is IPv4/IPv6 dependent, no elements of this
	 * structure and referred in create() may come after 'next'.
	 */
	struct mtype_elem next; /* temporary storage for uadd */
};

#define ipset_hash_elem_destroy_free(set, h, e) do {				\
	typeof(set) __set = (set);						\
	typeof(h) __h = (h);							\
	typeof(e) __e = (e);							\
	mtype_del_cidr_all(__set, __h, &__e->elem);				\
	ip_set_ext_destroy(__set, &__e->elem);					\
	kfree_rcu(__e, rcu);							\
} while (0)

#ifdef IP_SET_HASH_WITH_MULTI
#define ipset_hash_nelems(h) atomic_read(&(h)->rhlt.ht.nelems)
#define ipset_hash_walk_enter(h, iter)	rhltable_walk_enter(&(h)->rhlt, (iter))
#define ipset_hash_remove(h, e) rhltable_remove(&(h)->rhlt, &(e)->node, mtype_rht_params)
#else
#define ipset_hash_nelems(h) atomic_read(&(h)->ht.nelems)
#define ipset_hash_walk_enter(h, iter)	rhashtable_walk_enter(&(h)->ht, (iter))
#define ipset_hash_remove(h, e) rhashtable_remove_fast(&(h)->ht, &(e)->node, mtype_rht_params)
#endif

#ifdef IP_SET_HASH_WITH_NETS
/**
 * mtype_add_cidr - Add a CIDR entry to hash table bookkeeping
 * @set: Pointer to the ip_set
 * @h: Pointer to the htype
 * @cidr: The CIDR prefix length
 * @n: The index of the net_prefix array to add @cidr to
 *
 * Performs an update if @cidr is found, otherwise performs COW-style
 * allocation and replacement via RCU.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int
mtype_add_cidr(struct ip_set *set, struct htype *h, u8 cidr, u8 n)
{
	int i, j, found, len = 0, ret = 0;
	struct net_prefixes *nets, *tmp;
	struct net_prefix np;

	spin_lock_bh(&set->lock);
	nets = __ipset_dereference(h->rnets[n]);
	/* Add in increasing prefix order, so larger cidr first */
	for (i = 0, found = -1; i < nets->len; i++) {
		np = READ_ONCE(nets->nets[i]);
		if (np.count)
			len++;
		if (found != -1) {
			continue;
		} else if (np.cidr < cidr) {
			found = i;
		} else if (np.cidr == cidr) {
			if (np.count < CIDR_MAX_COUNT) {
				np.count++;
				WRITE_ONCE(nets->nets[i], np);
			} else {
				ret = -EOVERFLOW;
			}
			goto unlock;
		}
	}
	len++;
	tmp = kzalloc_flex(*tmp, nets, len, GFP_ATOMIC);
	if (!tmp) {
		ret = -ENOMEM;
		goto unlock;
	}

	tmp->len = len;
	seqcount_spinlock_init(&tmp->seq, &set->lock);
	for (i = 0, j = 0; i < nets->len; i++) {
		if (i == found) {
			tmp->nets[j].cidr = cidr;
			tmp->nets[j++].count = 1;
		}
		if (!nets->nets[i].count)
			continue;
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

/**
 * mtype_del_cidr - Remove CIDR entry and maintain array integrity.
 * @set: Pointer to the ip_set.
 * @h: Pointer to the htype.
 * @cidr: The CIDR prefix length.
 * @n: The index of the net_prefix array to remove @cidr from
 *
 * If CIDR entry count falls to 0, this function performs a "shift-left"
 * operation on all following elements. This ensures that the array remains
 * contiguous and maintains its descending order by CIDR. The vacated slot
 * at the end of the array is zeroed out (cidr=0, count=0).
 */
static void
mtype_del_cidr(struct ip_set *set, struct htype *h, u8 cidr, u8 n)
{
	struct net_prefixes *nets;
	struct net_prefix np;
	int found;
	u8 i, j;

	BUILD_BUG_ON(sizeof(struct net_prefix) != sizeof(u32));

	spin_lock_bh(&set->lock);
	nets = __ipset_dereference(h->rnets[n]);
	for (i = 0, found = -1; i < nets->len; i++) {
		np = READ_ONCE(nets->nets[i]);
		if (np.count && np.cidr == cidr) {
			np.count--;
			found = i;
			break;
		}
	}
	if (unlikely(found == -1))
		goto unlock;

	if (np.count) {
		WRITE_ONCE(nets->nets[found], np);
		goto unlock;
	}

	write_seqcount_begin(&nets->seq);
	for (i = 0, j = 0; i < nets->len; i++) {
		if (i == found)
			continue;

		np = READ_ONCE(nets->nets[i]);
		if (i != j)
			WRITE_ONCE(nets->nets[j], np);
		j++;
	}

	while (j < nets->len)
		WRITE_ONCE(nets->nets[j++], (struct net_prefix){});
	write_seqcount_end(&nets->seq);
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

/**
 * mtype_flush() - Flush a hash set type by destroying all elements.
 * @set: Pointer to the ip_set.
 *
 * Because other CPUs may concurrently insert new entries into the table
 * while flush is in progress, there is no guarantee that the table will
 * be empty upon return.
 */
static void
mtype_flush(struct ip_set *set)
{
	struct htype *h = set->data;
	struct rhashtable_iter hti;
	struct mtype_rht_elem *e;
	unsigned int dropped;

	ipset_hash_walk_enter(h, &hti);
restart:
	dropped = 0;
	rhashtable_walk_start(&hti);

	while ((e = rhashtable_walk_next(&hti))) {
		if (IS_ERR(e)) {
			if (PTR_ERR(e) == -EAGAIN)
				continue;
			break;
		}
		if (ipset_hash_remove(h, e))
			continue; /* Concurrent delete? skip */
		mtype_del_cidr_all(set, h, &e->elem);
		ip_set_ext_destroy(set, &e->elem);
		kfree_rcu(e, rcu);

		if (dropped++ > 128 && need_resched()) {
			rhashtable_walk_stop(&hti);
			cond_resched();
			goto restart;
		}
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

#ifdef IP_SET_HASH_WITH_MULTI
	rhltable_free_and_destroy(&h->rhlt, mtype_flush_elem, set);
#else
	rhashtable_free_and_destroy(&h->ht, mtype_flush_elem, set);
#endif

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++)
		kfree(rcu_dereference_raw(h->rnets[i]));
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
mtype_gc_do(struct ip_set *set)
{
	struct htype *h = set->data;
	struct rhashtable_iter hti;
	struct mtype_rht_elem *e;
	unsigned int seen;

	ipset_hash_walk_enter(h, &hti);
restart:
	seen = 0;
	rhashtable_walk_start(&hti);

	while ((e = rhashtable_walk_next(&hti))) {
		if (IS_ERR(e)) {
			if (PTR_ERR(e) == -EAGAIN)
				continue;
			break;
		}

		if (ip_set_timeout_expired(ext_timeout(&e->elem, set)) &&
		    ipset_hash_remove(h, e) == 0) {
			ip_set_ext_destroy(set, &e->elem);
			mtype_del_cidr_all(set, h, &e->elem);
			kfree_rcu(e, rcu);
		}

		if (seen++ > 128 && need_resched()) {
			/* stop+start can miss entries (we might have unlinked
			 * the element). This is harmless.
			 */
			rhashtable_walk_stop(&hti);
			cond_resched();
			goto restart;
		}
	}
	rhashtable_walk_stop(&hti);
	rhashtable_walk_exit(&hti);
}

static void
mtype_gc(struct work_struct *work)
{
	struct htable_gc *gc;
	struct ip_set *set;
	unsigned int next_run;

	gc = container_of(work, struct htable_gc, dwork.work);
	set = gc->set;

	next_run = IPSET_GC_PERIOD(set->timeout) * HZ;
	if (next_run < HZ)
		next_run = HZ;

	mtype_gc_do(set);

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
mtype_rht_size(struct ip_set *set, u32 *elements, size_t *ext_size)
{
	const struct htype *h = set->data;

	/* Do GC to collect expired elements now so that the reported
	 * element count doesn't include expired elements.
	 */
	if (SET_WITH_TIMEOUT(set))
		mtype_gc_do(set);

	*elements = ipset_hash_nelems(h);
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
	bool flag_exist = flags & IPSET_FLAG_EXIST;
	struct mtype_rht_elem *e, *old = NULL;
	const struct mtype_elem *d = value;
	struct htype *h = set->data;
	int ret = 0;
#ifdef IP_SET_HASH_WITH_NETS
	int i;
#endif

	rcu_read_lock_bh();
#ifdef IP_SET_HASH_WITH_MULTI
	{
		struct rhlist_head *tmp, *list;
		unsigned int seen = 0;
		u32 multi = 0;

		list = rhltable_lookup(&h->rhlt, d, mtype_rht_params);
		if (!list)
			goto insert;

		rhl_for_each_entry_rcu(old, tmp, list, node) {
			if (SET_ELEM_EXPIRED(set, &old->elem)) {
				if (rhltable_remove(&h->rhlt, &old->node, mtype_rht_params) == 0)
					ipset_hash_elem_destroy_free(set, h, old);
				continue;
			}

			if (mtype_data_equal(&old->elem, d, &multi))
				goto insert;
			++seen;
		}

		if (seen >= RHL_MAX_CHAINLEN) {
			ret = -IPSET_ERR_HASH_FULL;
			goto out_rcu_unlock;
		}
		old = NULL;
	}
#else
	old = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (!old)
		goto insert;
	/* simple case 1: element is expired, same as old == NULL. */
	if (SET_ELEM_EXPIRED(set, &old->elem)) {
		if (rhashtable_remove_fast(&h->ht, &old->node,
					   mtype_rht_params) == 0)
			ipset_hash_elem_destroy_free(set, h, old);
		old = NULL;
	}
#endif
insert:
	/* simple case 2: exists and we are not replacing.
	 * Can't do in-place extension updates, this can race with GC
	 * and mtype_del(): could end up adding new comment to an
	 * element that had kfree_rcu() called on it.
	 */
	if (old && !flag_exist) {
		ret = -IPSET_ERR_EXIST;
		goto out_rcu_unlock;
	}

	if (!old && ipset_hash_nelems(h) >= h->maxelem) {
		if (net_ratelimit())
			pr_warn("Set %s is full, maxelem %u reached\n",
				set->name, h->maxelem);
		ret = -IPSET_ERR_HASH_FULL;
		goto out_rcu_unlock;
	}

	e = kzalloc(offsetof(struct mtype_rht_elem, elem) + set->dsize,
		    GFP_ATOMIC);
	if (!e) {
		ret = -ENOMEM;
		goto out_rcu_unlock;
	}

	memcpy(&e->elem, d, sizeof(struct mtype_elem));

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++) {
		ret = mtype_add_cidr(set, h, DCIDR_GET(d->cidr, i), i);
		if (ret) {
			while (i > 0) {
				--i;
				mtype_del_cidr(set, h, DCIDR_GET(d->cidr, i), i);
			}
			kfree(e);
			goto out_rcu_unlock;
		}
	}

	mtype_data_set_flags(&e->elem, flags);
#endif
	if (SET_WITH_COUNTER(set))
		ip_set_init_counter(ext_counter(&e->elem, set), ext);
	if (SET_WITH_COMMENT(set) && !ext->target)
		ip_set_init_comment(set, ext_comment(&e->elem, set), ext);
	if (SET_WITH_SKBINFO(set))
		ip_set_init_skbinfo(ext_skbinfo(&e->elem, set), ext);
	if (SET_WITH_TIMEOUT(set))
		ip_set_timeout_set(ext_timeout(&e->elem, set), ext->timeout);

#ifdef IP_SET_HASH_WITH_MULTI
	spin_lock_bh(&set->lock);
	ret = rhltable_insert_key(&h->rhlt, &e->elem, &e->node, mtype_rht_params);
	if (ret == 0) {
		struct rhlist_head *list;

		/* rhltable_remove can fail here in case of parallel GC: harmless. */
		if (old && rhltable_remove(&h->rhlt, &old->node, mtype_rht_params) != 0)
			old = NULL;

		list = rhltable_lookup(&h->rhlt, d, mtype_rht_params);
		if (list) {
			const struct mtype_rht_elem *dup;
			struct rhlist_head *tmp;
			u32 multi = 0;

			rhl_for_each_entry_rcu(dup, tmp, list, node) {
				if (dup == e || !mtype_data_equal(&dup->elem, d, &multi))
					continue;

				/* check for duplicate key insertion: unlikely,
				 * but several CPUs can race to insert same key
				 * with same interface. We inserted e, but dup
				 * has same content: remove e again.
				 */
				if (rhltable_remove(&h->rhlt, &e->node, mtype_rht_params) == 0) {
					spin_unlock_bh(&set->lock);
					if (old) {
						ipset_hash_elem_destroy_free(set, h, old);
						old = NULL;
					}
					ret = -EEXIST;
					goto out_duplicate;
				}
			}
		}
	}
	spin_unlock_bh(&set->lock);
out_duplicate:
#else
	if (old)
		ret = rhashtable_replace_fast(&h->ht, &old->node, &e->node, mtype_rht_params);
	else
		ret = rhashtable_lookup_insert_key(&h->ht, d, &e->node, mtype_rht_params);

	if (ret == -ENOENT && old) {
		/* replace failed: maybe GC or concurrent mtype_del()
		 * reaped old element right now.
		 *
		 * Try once more with plain insert.
		 */
		old = NULL;
		ret = rhashtable_lookup_insert_key(&h->ht, d, &e->node, mtype_rht_params);
	}
#endif
	if (ret) {
		ip_set_ext_destroy(set, &e->elem);
		kfree_rcu(e, rcu);
		/* error: Undo mtype_add_cidr(). */
		mtype_del_cidr_all(set, h, d);
	} else if (old) {
		/* Update the new elements counters with the replaced one,
		 * unless userspace gave specific start values. old has
		 * been unlinked from table, but is still visible to other
		 * CPUs, so this is best-effort only.
		 */
		if (ext->packets == ULLONG_MAX &&
		    SET_WITH_COUNTER(set) &&
		    !SET_ELEM_EXPIRED(set, &old->elem)) {
			const struct ip_set_counter *old_c = ext_counter(&old->elem, set);
			struct ip_set_counter *new_c = ext_counter(&e->elem, set);

			atomic64_add(atomic64_read(&old_c->bytes), &new_c->bytes);
			atomic64_add(atomic64_read(&old_c->packets), &new_c->packets);
		}
		ip_set_ext_destroy(set, &old->elem);
		kfree_rcu(old, rcu);
		/* successful replace: Undo mtype_add_cidr(). */
		mtype_del_cidr_all(set, h, d);
	}

	if (ret == -EEXIST)
		ret = flag_exist ? 0 : -IPSET_ERR_EXIST;

	if (0)	/* to be removed */
		mtype_data_next(&h->next, d);
out_rcu_unlock:
	rcu_read_unlock_bh();
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

	rcu_read_lock_bh();
#ifdef IP_SET_HASH_WITH_MULTI
	{
		struct rhlist_head *tmp, *list;
		u32 multi = 0;

		list = rhltable_lookup(&h->rhlt, d, mtype_rht_params);
		if (!list)
			goto out_unlock;
		rhl_for_each_entry_rcu(e, tmp, list, node) {
			if (!mtype_data_equal(&e->elem, d, &multi))
				continue;
			if (SET_ELEM_EXPIRED(set, &e->elem))
				goto out_unlock;
			/* fails if GC evicts this entry right now. */
			ret = rhltable_remove(&h->rhlt, &e->node, mtype_rht_params);
			break;
		}

		/* either no entry found or race with GC */
		if (ret)
			goto out_unlock;
	}
#else
	e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (!e || SET_ELEM_EXPIRED(set, &e->elem))
		goto out_unlock;
	ret = rhashtable_remove_fast(&h->ht, &e->node, mtype_rht_params);
	if (ret)
		goto out_unlock;
#endif
	mtype_del_cidr_all(set, h, d);
	ip_set_ext_destroy(set, &e->elem);
	kfree_rcu(e, rcu);
out_unlock:
	rcu_read_unlock_bh();
	return ret ? -IPSET_ERR_EXIST : 0;
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
	unsigned int seq1;
	int ret, j, k;
#else
	int ret, j;
#endif
	unsigned int seq0;
	u32 multi;

	pr_debug("test by nets\n");
retry:
	multi = 0;
	nets0 = ipset_dereference_bh_nfnl(h->rnets[0]);
	seq0 = read_seqcount_begin(&nets0->seq);
#if IPSET_NET_COUNT == 2
	nets1 = ipset_dereference_bh_nfnl(h->rnets[1]);
	seq1 = read_seqcount_begin(&nets1->seq);
#endif
	for (j = 0; j < nets0->len && !multi; j++) {
		struct net_prefix p0 = READ_ONCE(nets0->nets[j]);

		if (!p0.count)
			continue;
#if IPSET_NET_COUNT == 2
		mtype_data_reset_elem(d, &orig);
		mtype_data_netmask(d, p0.cidr, false);
		for (k = 0; k < nets1->len && !multi; k++) {
			struct net_prefix p1 = READ_ONCE(nets1->nets[k]);

			if (!p1.count)
				continue;
			mtype_data_netmask(d, p1.cidr, true);
#else
		mtype_data_netmask(d, p0.cidr);
#endif
#ifdef IP_SET_HASH_WITH_MULTI
		{
		struct rhlist_head *tmp, *list;

		list = rhltable_lookup(&h->rhlt, d, mtype_rht_params);
		if (!list)
			continue;

		rhl_for_each_entry_rcu(e, tmp, list, node) {
			if (!SET_ELEM_EXPIRED(set, &e->elem))
				multi = true;
			if (!mtype_data_equal(&e->elem, d, &multi))
				continue;
			ret = mtype_data_match(&e->elem, ext, mext, set, flags);
			if (ret)
				goto check_retry;
			multi = false;
		}
		}
#else
		e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
		if (e) {
			if (!SET_ELEM_EXPIRED(set, &e->elem))
				multi = true;
			ret = mtype_data_match(&e->elem, ext, mext, set, flags);
			if (ret != 0)
				goto check_retry;
			multi = false;
		}
#endif
#if IPSET_NET_COUNT == 2
		}
#endif
	}
	ret = 0;
check_retry:
	if (read_seqcount_retry(&nets0->seq, seq0))
		goto retry;
#if IPSET_NET_COUNT == 2
	if (read_seqcount_retry(&nets1->seq, seq1))
		goto retry;
#endif
	return ret;
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

	rcu_read_lock_bh();
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

#ifdef IP_SET_HASH_WITH_MULTI
	{
		struct rhlist_head *tmp, *list;
		u32 multi = 0;

		list = rhltable_lookup(&h->rhlt, d, mtype_rht_params);
		if (!list)
			goto out;

		rhl_for_each_entry_rcu(e, tmp, list, node) {
			if (!mtype_data_equal(&e->elem, d, &multi))
				continue;
			ret = mtype_data_match(&e->elem, ext, mext, set, flags);
			if (ret)
				goto out;
		}
	}
#else
	e = rhashtable_lookup(&h->ht, d, mtype_rht_params);
	if (!e)
		goto out;

	ret = mtype_data_match(&e->elem, ext, mext, set, flags);
#endif
out:
	rcu_read_unlock_bh();
	return ret;
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

	mtype_rht_size(set, &elements, &ext_size);
	memsize = sizeof(*h) + ext_size + atomic64_read(&set->ext_size);

	nested = nla_nest_start(skb, IPSET_ATTR_DATA);
	if (!nested)
		goto nla_put_failure;

	if (nla_put_net32(skb, IPSET_ATTR_HASHSIZE, htonl(jhash_size(h->htable_bits))))
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
		if (nla_put_u8(skb, IPSET_ATTR_BUCKETSIZE, h->bucketsize))
			goto nla_put_failure;
		if (nla_put_net32(skb, IPSET_ATTR_INITVAL, htonl(h->initval)))
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
		hti = kmalloc_obj(*hti, GFP_ATOMIC);
		if (hti)
			ipset_hash_walk_enter(h, hti);
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
	u32 hashsize = IPSET_DEFAULT_HASHSIZE, maxelem = IPSET_DEFAULT_MAXELEM;
	struct rhashtable_params params;
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
		/* Not used anymore, just for backwards compat, old hash
		 * implementation used to -ENOMEM on too large hashsize.
		 */
		hashsize = ip_set_get_h32(tb[IPSET_ATTR_HASHSIZE]);
		if (hashsize > INT_MAX)
			return -ENOMEM;
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

	/* maxsize: maximum bucket table size to expand to */
	params.max_size = maxelem;

#ifdef IP_SET_HASH_WITH_MULTI
	err = rhltable_init(&h->rhlt, &params);
#else
	err = rhashtable_init(&h->ht, &params);
#endif
	if (err)
		goto free_h;

#ifdef IP_SET_HASH_WITH_NETS
	for (i = 0; i < IPSET_NET_COUNT; i++) {
		nets = kzalloc_obj(*nets);
		if (!nets) {
			while (i > 0)
				kfree(rcu_dereference_raw(h->rnets[--i]));
			goto free_rht;
		}
		seqcount_spinlock_init(&nets->seq, &set->lock);
		RCU_INIT_POINTER(h->rnets[i], nets);
	}
#endif
	h->gc.set = set;
	h->maxelem = maxelem;
#if defined(IP_SET_HASH_WITH_NETMASK) || defined(IP_SET_HASH_WITH_BITMASK)
	h->bitmask = bitmask;
	h->netmask = netmask;
#endif
#ifdef IP_SET_HASH_WITH_MARKMASK
	h->markmask = markmask;
#endif
	if (tb[IPSET_ATTR_INITVAL]) /* only stored for userspace dump compatibility */
		h->initval = ntohl(nla_get_be32(tb[IPSET_ATTR_INITVAL]));
	h->bucketsize = AHASH_MAX_SIZE;
	if (tb[IPSET_ATTR_BUCKETSIZE]) {
		h->bucketsize = nla_get_u8(tb[IPSET_ATTR_BUCKETSIZE]);
		if (h->bucketsize < AHASH_INIT_SIZE)
			h->bucketsize = AHASH_INIT_SIZE;
		else if (h->bucketsize > AHASH_MAX_SIZE)
			h->bucketsize = AHASH_MAX_SIZE;
		else if (h->bucketsize % 2)
			h->bucketsize += 1;
	}
	h->htable_bits = fls(hashsize - 1);
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
		 set->name, jhash_size(h->htable_bits), h->maxelem);

	return 0;

#ifdef IP_SET_HASH_WITH_NETS
free_rht:
#ifdef IP_SET_HASH_WITH_MULTI
	rhltable_free_and_destroy(&h->rhlt, mtype_flush_elem, set);
#else
	rhashtable_free_and_destroy(&h->ht, mtype_flush_elem, set);
#endif
#endif
free_h:
	kfree(h);
	return err ? err : -ENOMEM;
}
#endif /* IP_SET_EMIT_CREATE */

#undef HKEY_DATALEN
