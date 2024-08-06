/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef __LIBETH_TYPES_H
#define __LIBETH_TYPES_H

#include <linux/u64_stats_sync.h>

/**
 * struct libeth_netdev_priv - libeth netdev private structure
 * @curr_xdpsqs: current number of XDPSQs in use
 * @max_xdpsqs: maximum number of XDPSQs this netdev has
 * @last_rqs: number of RQs last time Ethtool stats were requested
 * @last_sqs: number of SQs last time Ethtool stats were requested
 * @last_xdpsqs: number of XDPSQ last time Ethtool stats were requested
 * @base_rqs: per-queue RQ stats containers with the netdev lifetime
 * @base_sqs: per-queue SQ stats containers with the netdev lifetime
 * @base_xdpsqs: per-queue XDPSQ stats containers with the netdev lifetime
 * @live_rqs: pointers to the current driver's embedded RQ stats
 * @live_sqs: pointers to the current driver's embedded SQ stats
 * @live_xdpsqs: pointers to the current driver's embedded XDPSQ stats
 *
 * The structure must be placed strictly at the beginning of driver's netdev
 * private structure if it uses libeth generic stats, as libeth uses
 * netdev_priv() to access it. The structure is private to libeth and
 * shouldn't be accessed from drivers directly.
 */
struct libeth_netdev_priv {
	u32				curr_xdpsqs;
	u32				max_xdpsqs;

	u16				last_rqs;
	u16				last_sqs;
	u16				last_xdpsqs;

	struct libeth_rq_base_stats	*base_rqs;
	struct libeth_sq_base_stats	*base_sqs;
	struct libeth_xdpsq_base_stats	*base_xdpsqs;

	const struct libeth_rq_stats	**live_rqs;
	const struct libeth_sq_stats	**live_sqs;
	const struct libeth_xdpsq_stats	**live_xdpsqs;

	/* Driver private data, ____cacheline_aligned */
} ____cacheline_aligned;

/**
 * libeth_netdev_priv_assert - assert the layout of driver's netdev priv struct
 * @t: typeof() of driver's netdev private structure
 * @f: name of the embedded &libeth_netdev_priv inside @t
 *
 * Make sure &libeth_netdev_priv is placed strictly at the beginning of
 * driver's private structure, so that libeth can use netdev_priv() to
 * access it.
 * To be called right after driver's netdev private struct declaration.
 */
#define libeth_netdev_priv_assert(t, f)					    \
	static_assert(__same_type(struct libeth_netdev_priv,		    \
				  typeof_member(t, f)) && !offsetof(t, f))

/* Stats. '[NL]' means it's exported to the Netlink per-queue stats */

/* Use 32-byte alignment to reduce false sharing. The first ~4 fields usually
 * are the hottest and the stats update helpers are unrolled by this count.
 */
#define __libeth_stats_aligned						    \
	__aligned(__cmp(min, 4 * sizeof(u64_stats_t), SMP_CACHE_BYTES))

/* Align queue stats counters naturally in case they aren't */
#define __libeth_u64_stats_t						    \
	u64_stats_t __aligned(sizeof(u64_stats_t))

#define ___live(s)			__libeth_u64_stats_t	s;

/* Rx per-queue stats:
 *
 * napi: "hot" counters, updated in bulks from NAPI polling loops:
 * bytes: bytes received on this queue [NL]
 * packets: packets received on this queue [NL]
 * fragments: number of processed descriptors carrying only a fragment
 * csum_unnecessary: number of frames the device checked the checksum for [NL]
 * hsplit: number of frames the device performed the header split for
 * hsplit_linear: number of frames placed entirely to the header buffer
 * hw_gro_packets: number of frames the device did HW GRO for [NL]
 * hw_gro_bytes: bytes for all HW GROed frames [NL]
 *
 * fail: "slow"/error counters, incremented by one when occurred:
 * alloc_fail: number of FQE (Rx buffer) allocation fails [NL]
 * dma_errs: number of hardware Rx DMA errors
 * csum_none: number of frames the device didn't check the checksum for [NL]
 * csum_bad: number of frames with invalid checksum [NL]
 * hsplit_errs: number of header split errors (header buffer overflows etc.)
 * build_fail: number of napi_build_skb() fails
 *
 * &libeth_rq_stats must be embedded into the corresponding queue structure.
 */

#define LIBETH_DECLARE_RQ_NAPI_STATS(act)				    \
	act(bytes)							    \
	act(packets)							    \
	act(fragments)							    \
	act(csum_unnecessary)						    \
	act(hsplit)							    \
	act(hsplit_linear)						    \
	act(hw_gro_packets)						    \
	act(hw_gro_bytes)

#define LIBETH_DECLARE_RQ_FAIL_STATS(act)				    \
	act(alloc_fail)							    \
	act(dma_errs)							    \
	act(csum_none)							    \
	act(csum_bad)							    \
	act(hsplit_errs)						    \
	act(build_fail)

#define LIBETH_DECLARE_RQ_STATS(act)					    \
	LIBETH_DECLARE_RQ_NAPI_STATS(act)				    \
	LIBETH_DECLARE_RQ_FAIL_STATS(act)

struct libeth_rq_stats {
	struct u64_stats_sync		syncp;

	union {
		struct {
			struct_group(napi,
				LIBETH_DECLARE_RQ_NAPI_STATS(___live);
			);
			LIBETH_DECLARE_RQ_FAIL_STATS(___live);
		};
		DECLARE_FLEX_ARRAY(__libeth_u64_stats_t, raw);
	};
} __libeth_stats_aligned;

/* Tx per-queue stats:
 *
 * napi: "hot" counters, updated in bulks from NAPI polling loops:
 * bytes: bytes sent from this queue [NL]
 * packets: packets sent from this queue [NL]
 *
 * xmit: "hot" counters, updated in bulks from ::ndo_start_xmit():
 * fragments: number of descriptors carrying only a fragment
 * csum_none: number of frames sent w/o checksum offload [NL]
 * needs_csum: number of frames sent with checksum offload [NL]
 * hw_gso_packets: number of frames sent with segmentation offload [NL]
 * tso: number of frames sent with TCP segmentation offload
 * uso: number of frames sent with UDP L4 segmentation offload
 * hw_gso_bytes: total bytes for HW GSOed frames [NL]
 *
 * fail: "slow"/error counters, incremented by one when occurred:
 * linearized: number of non-linear skbs linearized due to HW limits
 * dma_map_errs: number of DMA mapping errors
 * drops: number of skbs dropped by ::ndo_start_xmit()
 * busy: number of xmit failures due to the queue being full
 * stop: number of times the queue was stopped by the driver [NL]
 * wake: number of times the queue was started after being stopped [NL]
 *
 * &libeth_sq_stats must be embedded into the corresponding queue structure.
 */

#define LIBETH_DECLARE_SQ_NAPI_STATS(act)				    \
	act(bytes)							    \
	act(packets)

#define LIBETH_DECLARE_SQ_XMIT_STATS(act)				    \
	act(fragments)							    \
	act(csum_none)							    \
	act(needs_csum)							    \
	act(hw_gso_packets)						    \
	act(tso)							    \
	act(uso)							    \
	act(hw_gso_bytes)

#define LIBETH_DECLARE_SQ_FAIL_STATS(act)				    \
	act(linearized)							    \
	act(dma_map_errs)						    \
	act(drops)							    \
	act(busy)							    \
	act(stop)							    \
	act(wake)

#define LIBETH_DECLARE_SQ_STATS(act)					    \
	LIBETH_DECLARE_SQ_NAPI_STATS(act)				    \
	LIBETH_DECLARE_SQ_XMIT_STATS(act)				    \
	LIBETH_DECLARE_SQ_FAIL_STATS(act)

struct libeth_sq_stats {
	struct u64_stats_sync		syncp;

	union {
		struct {
			struct_group(napi,
				LIBETH_DECLARE_SQ_NAPI_STATS(___live);
			);
			struct_group(xmit,
				LIBETH_DECLARE_SQ_XMIT_STATS(___live);
			);
			LIBETH_DECLARE_SQ_FAIL_STATS(___live);
		};
		DECLARE_FLEX_ARRAY(__libeth_u64_stats_t, raw);
	};
} __libeth_stats_aligned;

/* XDP Tx per-queue stats:
 *
 * napi: "hot" counters, updated in bulks from NAPI polling loops:
 * bytes: bytes sent from this queue
 * packets: packets sent from this queue
 * fragments: number of descriptors carrying only a fragment
 *
 * fail: "slow"/error counters, incremented by one when occurred:
 * dma_map_errs: number of DMA mapping errors
 * drops: number of frags dropped due to the queue being full
 * busy: number of xmit failures due to the queue being full
 *
 * &libeth_xdpsq_stats must be embedded into the corresponding queue structure.
 */

#define LIBETH_DECLARE_XDPSQ_NAPI_STATS(act)				    \
	LIBETH_DECLARE_SQ_NAPI_STATS(act)				    \
	act(fragments)

#define LIBETH_DECLARE_XDPSQ_FAIL_STATS(act)				    \
	act(dma_map_errs)						    \
	act(drops)							    \
	act(busy)

#define LIBETH_DECLARE_XDPSQ_STATS(act)					    \
	LIBETH_DECLARE_XDPSQ_NAPI_STATS(act)				    \
	LIBETH_DECLARE_XDPSQ_FAIL_STATS(act)

struct libeth_xdpsq_stats {
	struct u64_stats_sync		syncp;

	union {
		struct {
			struct_group(napi,
				LIBETH_DECLARE_XDPSQ_NAPI_STATS(___live);
			);
			LIBETH_DECLARE_XDPSQ_FAIL_STATS(___live);
		};
		DECLARE_FLEX_ARRAY(__libeth_u64_stats_t, raw);
	};
} __libeth_stats_aligned;

#undef ___live

#endif /* __LIBETH_TYPES_H */
