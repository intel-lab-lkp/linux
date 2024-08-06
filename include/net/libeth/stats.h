/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef __LIBETH_STATS_H
#define __LIBETH_STATS_H

#include <linux/skbuff.h>
#include <linux/unroll.h>

#include <net/libeth/types.h>

/* Common */

/**
 * libeth_stats_inc_one - safely increment one stats structure counter
 * @s: queue stats structure to update (&libeth_rq_stats etc.)
 * @f: name of the field to increment
 *
 * To be used on exception or slow paths -- allocation fails, queue stops etc.
 */
#define libeth_stats_inc_one(s, f)					      \
	__libeth_stats_inc_one(s, f, __UNIQUE_ID(qs_))
#define __libeth_stats_inc_one(s, f, n) do {				      \
	typeof(*(s)) *n = (s);						      \
									      \
	u64_stats_update_begin(&n->syncp);				      \
	u64_stats_inc(&n->f);						      \
	u64_stats_update_end(&n->syncp);				      \
} while (0)

/**
 * libeth_stats_add_frags - update the frags counter if needed
 * @s: onstack stats structure to update (&libeth_rq_napi_stats etc.)
 * @frags: number of frags processed
 *
 * Update the frags counter if @frags > 1, do nothing for non-SG frames.
 */
#define libeth_stats_add_frags(s, frags)				      \
	__libeth_stats_add_frags(s, frags, __UNIQUE_ID(frags_))
#define __libeth_stats_add_frags(s, frags, uf) do {			      \
	u32 uf = (frags);						      \
									      \
	if (uf > 1)							      \
		(s)->fragments += uf;					      \
} while (0)

#define ___libeth_stats_add(qs, ss, group, uq, us, ur) do {		      \
	typeof(*(qs)) *uq = (qs);					      \
	u64_stats_t *ur = (typeof(ur))&uq->group;			      \
	typeof(*(ss)) *us = (ss);					      \
									      \
	static_assert(sizeof(uq->group) == sizeof(*us) * 2);		      \
	u64_stats_update_begin(&uq->syncp);				      \
									      \
	unrolled_count(__alignof(*uq) / sizeof(*uq->raw))		      \
	for (u32 i = 0; i < sizeof(*us) / sizeof(*us->raw); i++)	      \
		u64_stats_add(&ur[i], us->raw[i]);			      \
									      \
	u64_stats_update_end(&uq->syncp);				      \
} while (0)
#define __libeth_stats_add(qs, ss, group)				      \
	___libeth_stats_add(qs, ss, group, __UNIQUE_ID(qs_),		      \
			    __UNIQUE_ID(ss_), __UNIQUE_ID(raw_))

/* The following barely readable compression block defines the following
 * entities to be used in drivers:
 *
 * &libeth_rq_napi_stats - onstack stats container for RQ NAPI polling
 * libeth_rq_napi_stats_add() - add RQ onstack stats to the queue container
 * &libeth_sq_napi_stats - onstack stats container for SQ completion polling
 * libeth_sq_napi_stats_add() - add SQ onstack stats to the queue container
 * &libeth_sq_xmit_stats - onstack stats container for ::ndo_start_xmit()
 * libeth_sq_xmit_stats_add() - add SQ xmit stats to the queue container
 * &libeth_xdpsq_napi_stats - onstack stats container for XDPSQ polling
 * libeth_xdpsq_napi_stats_add() - add XDPSQ stats to the queue container
 *
 * During the NAPI poll loop or any other hot function, the "hot" counters
 * get updated on the stack only. Then at the end, the corresponding _add()
 * is called to quickly add them to the stats container embedded into the
 * queue structure using __libeth_stats_add().
 * The onstack counters are of type u32, thus it is assumed that one
 * polling/sending cycle can't go above ``U32_MAX`` for any of them.
 */

#define ___stack(s)		u32	s;

#define LIBETH_STATS_DEFINE_STACK(pfx, PFX, type, TYPE)			      \
struct libeth_##pfx##_##type##_stats {					      \
	union {								      \
		struct {						      \
			LIBETH_DECLARE_##PFX##_##TYPE##_STATS(___stack);      \
		};							      \
		DECLARE_FLEX_ARRAY(u32, raw);				      \
	};								      \
};									      \
									      \
static inline void							      \
libeth_##pfx##_##type##_stats_add(struct libeth_##pfx##_stats *qs,	      \
				  const struct libeth_##pfx##_##type##_stats  \
				  *ss)					      \
{									      \
	__libeth_stats_add(qs, ss, type);				      \
}

#define LIBETH_STATS_DECLARE_HELPERS(pfx)				      \
void libeth_##pfx##_stats_init(const struct net_device *dev,		      \
			       struct libeth_##pfx##_stats *stats,	      \
			       u32 qid);				      \
void libeth_##pfx##_stats_deinit(const struct net_device *dev, u32 qid)

LIBETH_STATS_DEFINE_STACK(rq, RQ, napi, NAPI);
LIBETH_STATS_DECLARE_HELPERS(rq);

LIBETH_STATS_DEFINE_STACK(sq, SQ, napi, NAPI);
LIBETH_STATS_DEFINE_STACK(sq, SQ, xmit, XMIT);

/**
 * libeth_sq_xmit_stats_csum - convert skb csum status to the SQ xmit stats
 * @ss: onstack SQ xmit stats to increment
 * @skb: &sk_buff to process stats for
 *
 * To be called from ::ndo_start_xmit() to account whether checksum offload
 * was enabled when sending this @skb.
 */
static inline void libeth_sq_xmit_stats_csum(struct libeth_sq_xmit_stats *ss,
					     const struct sk_buff *skb)
{
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		ss->needs_csum++;
	else
		ss->csum_none++;
}

LIBETH_STATS_DECLARE_HELPERS(sq);

LIBETH_STATS_DEFINE_STACK(xdpsq, XDPSQ, napi, NAPI);
LIBETH_STATS_DECLARE_HELPERS(xdpsq);

#undef ___stack

#endif /* __LIBETH_STATS_H */
