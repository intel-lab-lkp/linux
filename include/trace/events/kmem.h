/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kmem

#if !defined(_TRACE_KMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KMEM_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <trace/events/mmflags.h>

TRACE_EVENT(kmem_cache_alloc,

	TP_PROTO(unsigned long call_site,
		 const void *ptr,
		 struct kmem_cache *s,
		 gfp_t gfp_flags,
		 int node),

	TP_ARGS(call_site, ptr, s, gfp_flags, node),

	TP_STRUCT__entry(
		__field(	unsigned long,	call_site	)
		__field(	const void *,	ptr		)
		__string(	name,		s->name		)
		__field(	size_t,		bytes_req	)
		__field(	size_t,		bytes_alloc	)
		__field(	unsigned long,	gfp_flags	)
		__field(	int,		node		)
		__field(	bool,		accounted	)
	),

	TP_fast_assign(
		__entry->call_site	= call_site;
		__entry->ptr		= ptr;
		__assign_str(name);
		__entry->bytes_req	= s->object_size;
		__entry->bytes_alloc	= s->size;
		__entry->gfp_flags	= (__force unsigned long)gfp_flags;
		__entry->node		= node;
		__entry->accounted	= IS_ENABLED(CONFIG_MEMCG) ?
					  ((gfp_flags & __GFP_ACCOUNT) ||
					  (s->flags & SLAB_ACCOUNT)) : false;
	),

	TP_printk("call_site=%pS ptr=%p name=%s bytes_req=%zu bytes_alloc=%zu gfp_flags=%s node=%d accounted=%s",
		(void *)__entry->call_site,
		__entry->ptr,
		__get_str(name),
		__entry->bytes_req,
		__entry->bytes_alloc,
		show_gfp_flags(__entry->gfp_flags),
		__entry->node,
		__entry->accounted ? "true" : "false")
);

TRACE_EVENT(kmalloc,

	TP_PROTO(unsigned long call_site,
		 const void *ptr,
		 size_t bytes_req,
		 size_t bytes_alloc,
		 gfp_t gfp_flags,
		 int node),

	TP_ARGS(call_site, ptr, bytes_req, bytes_alloc, gfp_flags, node),

	TP_STRUCT__entry(
		__field(	unsigned long,	call_site	)
		__field(	const void *,	ptr		)
		__field(	size_t,		bytes_req	)
		__field(	size_t,		bytes_alloc	)
		__field(	unsigned long,	gfp_flags	)
		__field(	int,		node		)
	),

	TP_fast_assign(
		__entry->call_site	= call_site;
		__entry->ptr		= ptr;
		__entry->bytes_req	= bytes_req;
		__entry->bytes_alloc	= bytes_alloc;
		__entry->gfp_flags	= (__force unsigned long)gfp_flags;
		__entry->node		= node;
	),

	TP_printk("call_site=%pS ptr=%p bytes_req=%zu bytes_alloc=%zu gfp_flags=%s node=%d accounted=%s",
		(void *)__entry->call_site,
		__entry->ptr,
		__entry->bytes_req,
		__entry->bytes_alloc,
		show_gfp_flags(__entry->gfp_flags),
		__entry->node,
		(IS_ENABLED(CONFIG_MEMCG) &&
		 (__entry->gfp_flags & (__force unsigned long)__GFP_ACCOUNT)) ? "true" : "false")
);

TRACE_EVENT(kfree,

	TP_PROTO(unsigned long call_site, const void *ptr),

	TP_ARGS(call_site, ptr),

	TP_STRUCT__entry(
		__field(	unsigned long,	call_site	)
		__field(	const void *,	ptr		)
	),

	TP_fast_assign(
		__entry->call_site	= call_site;
		__entry->ptr		= ptr;
	),

	TP_printk("call_site=%pS ptr=%p",
		  (void *)__entry->call_site, __entry->ptr)
);

TRACE_EVENT(kmem_cache_free,

	TP_PROTO(unsigned long call_site, const void *ptr, const struct kmem_cache *s),

	TP_ARGS(call_site, ptr, s),

	TP_STRUCT__entry(
		__field(	unsigned long,	call_site	)
		__field(	const void *,	ptr		)
		__string(	name,		s->name		)
	),

	TP_fast_assign(
		__entry->call_site	= call_site;
		__entry->ptr		= ptr;
		__assign_str(name);
	),

	TP_printk("call_site=%pS ptr=%p name=%s",
		  (void *)__entry->call_site, __entry->ptr, __get_str(name))
);

TRACE_EVENT(mm_page_free,

	TP_PROTO(struct page *page, unsigned int order),

	TP_ARGS(page, order),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
		__field(	unsigned int,	order		)
	),

	TP_fast_assign(
		__entry->pfn		= page_to_pfn(page);
		__entry->order		= order;
	),

	TP_printk("page=%p pfn=0x%lx order=%d",
			pfn_to_page(__entry->pfn),
			__entry->pfn,
			__entry->order)
);

TRACE_EVENT(mm_page_free_batched,

	TP_PROTO(struct page *page),

	TP_ARGS(page),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
	),

	TP_fast_assign(
		__entry->pfn		= page_to_pfn(page);
	),

	TP_printk("page=%p pfn=0x%lx order=0",
			pfn_to_page(__entry->pfn),
			__entry->pfn)
);

TRACE_EVENT(mm_page_alloc,

	TP_PROTO(struct page *page, unsigned int order,
			gfp_t gfp_flags, int migratetype),

	TP_ARGS(page, order, gfp_flags, migratetype),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
		__field(	unsigned int,	order		)
		__field(	unsigned long,	gfp_flags	)
		__field(	int,		migratetype	)
	),

	TP_fast_assign(
		__entry->pfn		= page ? page_to_pfn(page) : -1UL;
		__entry->order		= order;
		__entry->gfp_flags	= (__force unsigned long)gfp_flags;
		__entry->migratetype	= migratetype;
	),

	TP_printk("page=%p pfn=0x%lx order=%d migratetype=%d gfp_flags=%s",
		__entry->pfn != -1UL ? pfn_to_page(__entry->pfn) : NULL,
		__entry->pfn != -1UL ? __entry->pfn : 0,
		__entry->order,
		__entry->migratetype,
		show_gfp_flags(__entry->gfp_flags))
);

DECLARE_EVENT_CLASS(mm_page,

	TP_PROTO(struct page *page, unsigned int order, int migratetype,
		 int percpu_refill),

	TP_ARGS(page, order, migratetype, percpu_refill),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
		__field(	unsigned int,	order		)
		__field(	int,		migratetype	)
		__field(	int,		percpu_refill	)
	),

	TP_fast_assign(
		__entry->pfn		= page ? page_to_pfn(page) : -1UL;
		__entry->order		= order;
		__entry->migratetype	= migratetype;
		__entry->percpu_refill	= percpu_refill;
	),

	TP_printk("page=%p pfn=0x%lx order=%u migratetype=%d percpu_refill=%d",
		__entry->pfn != -1UL ? pfn_to_page(__entry->pfn) : NULL,
		__entry->pfn != -1UL ? __entry->pfn : 0,
		__entry->order,
		__entry->migratetype,
		__entry->percpu_refill)
);

DEFINE_EVENT(mm_page, mm_page_alloc_zone_locked,

	TP_PROTO(struct page *page, unsigned int order, int migratetype,
		 int percpu_refill),

	TP_ARGS(page, order, migratetype, percpu_refill)
);

TRACE_EVENT(mm_page_pcpu_drain,

	TP_PROTO(struct page *page, unsigned int order, int migratetype),

	TP_ARGS(page, order, migratetype),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
		__field(	unsigned int,	order		)
		__field(	int,		migratetype	)
	),

	TP_fast_assign(
		__entry->pfn		= page ? page_to_pfn(page) : -1UL;
		__entry->order		= order;
		__entry->migratetype	= migratetype;
	),

	TP_printk("page=%p pfn=0x%lx order=%d migratetype=%d",
		pfn_to_page(__entry->pfn), __entry->pfn,
		__entry->order, __entry->migratetype)
);

/*
 * spb_pb_taint action encoding.
 */
#define SPB_PB_TAINT_ACTION_SET		0   /* set PB_has_<mt> */
#define SPB_PB_TAINT_ACTION_CLEAR	1   /* clear PB_has_<mt> */

#define show_spb_pb_taint_action(a)				\
	__print_symbolic(a,					\
		{ SPB_PB_TAINT_ACTION_SET,	"SET"   },	\
		{ SPB_PB_TAINT_ACTION_CLEAR,	"CLEAR" })

/*
 * Per-call tracepoint at every PB_has_<migratetype> bit transition.
 * Distinct from the existing trace_printk lines (which only fire on
 * the FIRST 0->1 transition per (SPB, migratetype)) — this fires on
 * EVERY successful set/clear, and includes a flag for whether this
 * call also caused a 0<->1 transition at the SPB-level counter
 * (i.e., is_first_or_last for this (SPB, mt) combination).
 *
 * Use to answer "who is painting/clearing PB_has bits and at what
 * rate?" — most useful when investigating runaway tainting or when
 * Stage 1 / sync evac should be clearing bits but isn't.
 *
 * High volume: bounded by the rate of PB_has_* bit changes, which
 * is typically per-allocation. Static-key gated to zero overhead
 * when detached.
 */
TRACE_EVENT(spb_pb_taint,

	TP_PROTO(struct page *page, int migratetype, int action,
		 bool is_first_or_last),

	TP_ARGS(page, migratetype, action, is_first_or_last),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn			)
		__field(	int,		migratetype		)
		__field(	int,		action			)
		__field(	bool,		is_first_or_last	)
	),

	TP_fast_assign(
		__entry->pfn			= page_to_pfn(page);
		__entry->migratetype		= migratetype;
		__entry->action			= action;
		__entry->is_first_or_last	= is_first_or_last;
	),

	TP_printk("pfn=0x%lx mt=%d action=%s first_or_last=%d",
		__entry->pfn,
		__entry->migratetype,
		show_spb_pb_taint_action(__entry->action),
		__entry->is_first_or_last)
);

/*
 * spb_claim_block_refused reason encoding.
 */
#define SPB_CLAIM_REFUSED_ISOLATE		0
#define SPB_CLAIM_REFUSED_CMA			1
#define SPB_CLAIM_REFUSED_ZONE_BOUNDARY		2
#define SPB_CLAIM_REFUSED_CROSS_TYPE_NOT_FREE	3
#define SPB_CLAIM_REFUSED_INSUFFICIENT_COMPAT	4

#define show_spb_claim_refused_reason(r)				\
	__print_symbolic(r,						\
		{ SPB_CLAIM_REFUSED_ISOLATE,         "ISOLATE"        },	\
		{ SPB_CLAIM_REFUSED_CMA,             "CMA"            },	\
		{ SPB_CLAIM_REFUSED_ZONE_BOUNDARY,   "ZONE_BOUNDARY"  },	\
		{ SPB_CLAIM_REFUSED_CROSS_TYPE_NOT_FREE, "CROSS_TYPE_NOT_FREE" }, \
		{ SPB_CLAIM_REFUSED_INSUFFICIENT_COMPAT, "INSUFFICIENT_COMPAT" })

/*
 * Per-refusal tracepoint inside try_to_claim_block. The function can
 * fail for several reasons: pageblock isolated for evacuation, CMA
 * pageblock, zone boundary straddle, cross-type relabel that requires
 * a fully-free PB, or the heuristic threshold that says too few pages
 * in the block are compatible. Visibility into WHICH reason fires how
 * often informs Stage 4 design (e.g., is the heuristic gate the
 * dominant cause of allocations spilling to clean SPBs?).
 *
 * Volume: bounded by the rate of fallback attempts, which is rare
 * compared to total allocations.
 */
TRACE_EVENT(spb_claim_block_refused,

	TP_PROTO(struct page *page, int start_type, int block_type,
		 int reason),

	TP_ARGS(page, start_type, block_type, reason),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn		)
		__field(	int,		start_type	)
		__field(	int,		block_type	)
		__field(	int,		reason		)
	),

	TP_fast_assign(
		__entry->pfn		= page_to_pfn(page);
		__entry->start_type	= start_type;
		__entry->block_type	= block_type;
		__entry->reason		= reason;
	),

	TP_printk("pfn=0x%lx start_mt=%d block_mt=%d reason=%s",
		__entry->pfn,
		__entry->start_type,
		__entry->block_type,
		show_spb_claim_refused_reason(__entry->reason))
);

/*
 * Per-call tracepoint at the exit of spb_evacuate_for_order, the
 * synchronous slowpath evacuator called from
 * __alloc_pages_direct_compact. Captures how many evacuate_pageblock
 * calls were attempted in each phase:
 *   - Phase 1: coalesce within existing same-mt pageblocks
 *   - Phase 2: evacuate whole movable pageblocks to create free PBs
 *
 * Together with pgmigrate_success/pgmigrate_fail counter deltas, this
 * lets us answer "is slowpath sync evacuation actually creating
 * useful free pageblocks, or are the migrations EAGAINing on busy
 * ebs?" — directly informs whether the per-call budget caps need
 * tuning.
 *
 * Low volume: ~one event per direct-compact slowpath visit.
 */
TRACE_EVENT(spb_evacuate_for_order_done,

	TP_PROTO(struct zone *zone, unsigned int order, int migratetype,
		 unsigned int phase1_attempts, unsigned int phase2_attempts,
		 bool did_evacuate),

	TP_ARGS(zone, order, migratetype, phase1_attempts,
		phase2_attempts, did_evacuate),

	TP_STRUCT__entry(
		__string(	name,			zone->name	)
		__field(	unsigned int,		order		)
		__field(	int,			migratetype	)
		__field(	unsigned int,		phase1_attempts	)
		__field(	unsigned int,		phase2_attempts	)
		__field(	bool,			did_evacuate	)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->order			= order;
		__entry->migratetype		= migratetype;
		__entry->phase1_attempts	= phase1_attempts;
		__entry->phase2_attempts	= phase2_attempts;
		__entry->did_evacuate		= did_evacuate;
	),

	TP_printk("zone=%s order=%u mt=%d p1=%u p2=%u did_evac=%d",
		__get_str(name),
		__entry->order,
		__entry->migratetype,
		__entry->phase1_attempts,
		__entry->phase2_attempts,
		__entry->did_evacuate)
);

/*
 * spb_alloc_atomic_relax step encoding.
 */
#define SPB_ATOMIC_RELAX_NORETRY_SKIP	0   /* NORETRY caller — return NULL */
#define SPB_ATOMIC_RELAX_ADD_TAINTED_OK	1   /* add ALLOC_NOFRAG_TAINTED_OK retry */
#define SPB_ATOMIC_RELAX_DROP_NOFRAGMENT 2  /* drop ALLOC_NOFRAGMENT retry */

#define show_spb_atomic_relax_step(s)					\
	__print_symbolic(s,						\
		{ SPB_ATOMIC_RELAX_NORETRY_SKIP,    "NORETRY_SKIP"    }, \
		{ SPB_ATOMIC_RELAX_ADD_TAINTED_OK,  "ADD_TAINTED_OK"  }, \
		{ SPB_ATOMIC_RELAX_DROP_NOFRAGMENT, "DROP_NOFRAGMENT" })

/*
 * Per-event tracepoint at each atomic-allocation NOFRAGMENT-relaxation
 * step in get_page_from_freelist. Captures NORETRY-skip exits (caller
 * had a fallback so we returned NULL), and the two relaxation retries
 * (add NOFRAG_TAINTED_OK; drop NOFRAGMENT entirely).
 *
 * Use to quantify how often each step fires under the workload.
 * Validates the NORETRY-skip change is paying off.
 *
 * Volume: only on atomic allocs that exhaust the tainted pool —
 * typically rare on a healthy system.
 */
TRACE_EVENT(spb_alloc_atomic_relax,

	TP_PROTO(struct zone *zone, unsigned int order, int migratetype,
		 gfp_t gfp_mask, int step),

	TP_ARGS(zone, order, migratetype, gfp_mask, step),

	TP_STRUCT__entry(
		__string(	name,			zone->name	)
		__field(	unsigned int,		order		)
		__field(	int,			migratetype	)
		__field(	unsigned long,		gfp_mask	)
		__field(	int,			step		)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->order		= order;
		__entry->migratetype	= migratetype;
		__entry->gfp_mask	= (__force unsigned long)gfp_mask;
		__entry->step		= step;
	),

	TP_printk("zone=%s order=%u mt=%d gfp=%s step=%s",
		__get_str(name),
		__entry->order,
		__entry->migratetype,
		show_gfp_flags(__entry->gfp_mask),
		show_spb_atomic_relax_step(__entry->step))
);

/*
 * spb_alloc_walk outcome encoding. SUCCESS_* values name which Pass
 * inside __rmqueue_smallest produced the page. NO_PAGE means the
 * function returned NULL (all passes failed).
 */
#define SPB_ALLOC_OUTCOME_NO_PAGE	0
#define SPB_ALLOC_OUTCOME_PASS_1	1   /* preferred SPBs */
#define SPB_ALLOC_OUTCOME_PASS_2	2   /* claim_whole_block from tainted */
#define SPB_ALLOC_OUTCOME_PASS_2B	3   /* sub-PB claim from tainted */
#define SPB_ALLOC_OUTCOME_PASS_2C	4   /* cross-non-movable borrow */
#define SPB_ALLOC_OUTCOME_PASS_3	5   /* empty SPB (taints fresh SPB) */
#define SPB_ALLOC_OUTCOME_PASS_4	6   /* movable falls back to tainted */
#define SPB_ALLOC_OUTCOME_ZONE_FALLBACK	7  /* zone-level free_area (hotplug edge) */
#define SPB_ALLOC_OUTCOME_PASS_2D	8   /* cross-MOV borrow within tainted */

#define show_spb_alloc_outcome(o)				\
	__print_symbolic(o,					\
		{ SPB_ALLOC_OUTCOME_NO_PAGE,	"NO_PAGE"  },	\
		{ SPB_ALLOC_OUTCOME_PASS_1,	"PASS_1"   },	\
		{ SPB_ALLOC_OUTCOME_PASS_2,	"PASS_2"   },	\
		{ SPB_ALLOC_OUTCOME_PASS_2B,	"PASS_2B"  },	\
		{ SPB_ALLOC_OUTCOME_PASS_2C,	"PASS_2C"  },	\
		{ SPB_ALLOC_OUTCOME_PASS_2D,	"PASS_2D"  },	\
		{ SPB_ALLOC_OUTCOME_PASS_3,	"PASS_3"   },	\
		{ SPB_ALLOC_OUTCOME_PASS_4,	"PASS_4"   },	\
		{ SPB_ALLOC_OUTCOME_ZONE_FALLBACK, "ZONE_FB" })

/*
 * Per-allocation tracepoint at every exit of __rmqueue_smallest.
 * Captures how many SPBs were walked before the allocation was
 * satisfied (or determined unsatisfiable).
 *
 * Use this to characterize the cost of the linear spb_lists walk:
 *   - typical walk depth per allocation
 *   - per-(order, migratetype) walk-depth distribution
 *   - whether some workloads see pathologically long walks
 *
 * High-volume tracepoint (~1 emission per allocation, ~hundreds of
 * thousands per second on busy systems). The static-key gating in
 * the caller keeps cost at ~1 ns when the tracepoint is detached.
 * When attached, expect ~100 ns/event (~10% CPU on a saturated
 * allocator). Filter by outcome to reduce volume:
 *   tracepoint:kmem:spb_alloc_walk /args->n_spbs_visited > 5/ { ... }
 */
TRACE_EVENT(spb_alloc_walk,

	TP_PROTO(struct zone *zone, unsigned int order, int migratetype,
		 unsigned int alloc_flags, int outcome,
		 unsigned int n_spbs_visited),

	TP_ARGS(zone, order, migratetype, alloc_flags, outcome,
		n_spbs_visited),

	TP_STRUCT__entry(
		__string(	name,			zone->name	)
		__field(	unsigned int,		order		)
		__field(	int,			migratetype	)
		__field(	unsigned int,		alloc_flags	)
		__field(	int,			outcome		)
		__field(	unsigned int,		n_spbs_visited	)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->order			= order;
		__entry->migratetype		= migratetype;
		__entry->alloc_flags		= alloc_flags;
		__entry->outcome		= outcome;
		__entry->n_spbs_visited		= n_spbs_visited;
	),

	TP_printk("zone=%s order=%u mt=%d alloc_flags=0x%x outcome=%s n_spbs_visited=%u",
		__get_str(name),
		__entry->order,
		__entry->migratetype,
		__entry->alloc_flags,
		show_spb_alloc_outcome(__entry->outcome),
		__entry->n_spbs_visited)
);

/*
 * Diagnostic tracepoint fired when __rmqueue_smallest's tainted-SPB
 * passes (Pass 1/2/2b/2c) all failed and the allocator is about to
 * fall through to Pass 3 (which may taint a clean SPB) or to the
 * fallback paths in __rmqueue_claim/__rmqueue_steal.
 *
 * Captures enough state to answer "why didn't an existing tainted SPB
 * absorb this allocation?":
 *   - n_tainted_with_buddy: count of tainted SPBs whose free_area at
 *     the requested order has a non-empty free_list of the requested
 *     migratetype. >0 means buddies WERE available — Pass 1 missed
 *     them somehow. 0 means the tainted pool genuinely had nothing at
 *     the right (order, mt).
 *   - walk flags: snapshot of struct spb_tainted_walk gathered during
 *     Pass 1's walk. saw_free_pages = any tainted SPB had any free
 *     pages anywhere; saw_free_pb = any tainted SPB had a wholly-free
 *     pageblock; saw_below_reserve = any tainted SPB was at or below
 *     its reserve threshold.
 *
 * Fires once per fall-through event, so volume scales with the rate
 * at which clean-SPB tainting becomes a possibility — typically rare
 * once the workload reaches steady state.
 */
TRACE_EVENT(spb_alloc_fall_through,

	TP_PROTO(struct zone *zone, unsigned int order, int migratetype,
		 unsigned int alloc_flags,
		 unsigned int n_tainted, unsigned int n_tainted_with_buddy,
		 bool saw_free_pages, bool saw_free_pb,
		 bool saw_below_reserve),

	TP_ARGS(zone, order, migratetype, alloc_flags,
		n_tainted, n_tainted_with_buddy,
		saw_free_pages, saw_free_pb, saw_below_reserve),

	TP_STRUCT__entry(
		__string(	name,			zone->name		)
		__field(	unsigned int,		order			)
		__field(	int,			migratetype		)
		__field(	unsigned int,		alloc_flags		)
		__field(	unsigned int,		n_tainted		)
		__field(	unsigned int,		n_tainted_with_buddy	)
		__field(	bool,			saw_free_pages		)
		__field(	bool,			saw_free_pb		)
		__field(	bool,			saw_below_reserve	)
	),

	TP_fast_assign(
		__assign_str(name);
		__entry->order			= order;
		__entry->migratetype		= migratetype;
		__entry->alloc_flags		= alloc_flags;
		__entry->n_tainted		= n_tainted;
		__entry->n_tainted_with_buddy	= n_tainted_with_buddy;
		__entry->saw_free_pages		= saw_free_pages;
		__entry->saw_free_pb		= saw_free_pb;
		__entry->saw_below_reserve	= saw_below_reserve;
	),

	TP_printk("zone=%s order=%u mt=%d alloc_flags=0x%x n_tainted=%u n_tainted_with_buddy=%u walk=[fp=%d fpb=%d below=%d]",
		__get_str(name),
		__entry->order,
		__entry->migratetype,
		__entry->alloc_flags,
		__entry->n_tainted,
		__entry->n_tainted_with_buddy,
		__entry->saw_free_pages,
		__entry->saw_free_pb,
		__entry->saw_below_reserve)
);

TRACE_EVENT(mm_page_alloc_extfrag,

	TP_PROTO(struct page *page,
		int alloc_order, int fallback_order,
		int alloc_migratetype, int fallback_migratetype),

	TP_ARGS(page,
		alloc_order, fallback_order,
		alloc_migratetype, fallback_migratetype),

	TP_STRUCT__entry(
		__field(	unsigned long,	pfn			)
		__field(	int,		alloc_order		)
		__field(	int,		fallback_order		)
		__field(	int,		alloc_migratetype	)
		__field(	int,		fallback_migratetype	)
		__field(	int,		change_ownership	)
	),

	TP_fast_assign(
		__entry->pfn			= page_to_pfn(page);
		__entry->alloc_order		= alloc_order;
		__entry->fallback_order		= fallback_order;
		__entry->alloc_migratetype	= alloc_migratetype;
		__entry->fallback_migratetype	= fallback_migratetype;
		__entry->change_ownership	= (alloc_migratetype ==
					get_pageblock_migratetype(page));
	),

	TP_printk("page=%p pfn=0x%lx alloc_order=%d fallback_order=%d pageblock_order=%d alloc_migratetype=%d fallback_migratetype=%d fragmenting=%d change_ownership=%d",
		pfn_to_page(__entry->pfn),
		__entry->pfn,
		__entry->alloc_order,
		__entry->fallback_order,
		pageblock_order,
		__entry->alloc_migratetype,
		__entry->fallback_migratetype,
		__entry->fallback_order < pageblock_order,
		__entry->change_ownership)
);

TRACE_EVENT(mm_setup_per_zone_wmarks,

	TP_PROTO(struct zone *zone),

	TP_ARGS(zone),

	TP_STRUCT__entry(
		__field(int, node_id)
		__string(name, zone->name)
		__field(unsigned long, watermark_min)
		__field(unsigned long, watermark_low)
		__field(unsigned long, watermark_high)
		__field(unsigned long, watermark_promo)
	),

	TP_fast_assign(
		__entry->node_id = zone->zone_pgdat->node_id;
		__assign_str(name);
		__entry->watermark_min = zone->_watermark[WMARK_MIN];
		__entry->watermark_low = zone->_watermark[WMARK_LOW];
		__entry->watermark_high = zone->_watermark[WMARK_HIGH];
		__entry->watermark_promo = zone->_watermark[WMARK_PROMO];
	),

	TP_printk("node_id=%d zone name=%s watermark min=%lu low=%lu high=%lu promo=%lu",
		  __entry->node_id,
		  __get_str(name),
		  __entry->watermark_min,
		  __entry->watermark_low,
		  __entry->watermark_high,
		  __entry->watermark_promo)
);

TRACE_EVENT(mm_setup_per_zone_lowmem_reserve,

	TP_PROTO(struct zone *zone, struct zone *upper_zone, long lowmem_reserve),

	TP_ARGS(zone, upper_zone, lowmem_reserve),

	TP_STRUCT__entry(
		__field(int, node_id)
		__string(name, zone->name)
		__string(upper_name, upper_zone->name)
		__field(long, lowmem_reserve)
	),

	TP_fast_assign(
		__entry->node_id = zone->zone_pgdat->node_id;
		__assign_str(name);
		__assign_str(upper_name);
		__entry->lowmem_reserve = lowmem_reserve;
	),

	TP_printk("node_id=%d zone name=%s upper_zone name=%s lowmem_reserve_pages=%ld",
		  __entry->node_id,
		  __get_str(name),
		  __get_str(upper_name),
		  __entry->lowmem_reserve)
);

TRACE_EVENT(mm_calculate_totalreserve_pages,

	TP_PROTO(unsigned long totalreserve_pages),

	TP_ARGS(totalreserve_pages),

	TP_STRUCT__entry(
		__field(unsigned long, totalreserve_pages)
	),

	TP_fast_assign(
		__entry->totalreserve_pages = totalreserve_pages;
	),

	TP_printk("totalreserve_pages=%lu", __entry->totalreserve_pages)
);


/*
 * Required for uniquely and securely identifying mm in rss_stat tracepoint.
 */
#ifndef __PTR_TO_HASHVAL
static unsigned int __maybe_unused mm_ptr_to_hash(const void *ptr)
{
	int ret;
	unsigned long hashval;

	ret = ptr_to_hashval(ptr, &hashval);
	if (ret)
		return 0;

	/* The hashed value is only 32-bit */
	return (unsigned int)hashval;
}
#define __PTR_TO_HASHVAL
#endif

#define TRACE_MM_PAGES		\
	EM(MM_FILEPAGES)	\
	EM(MM_ANONPAGES)	\
	EM(MM_SWAPENTS)		\
	EMe(MM_SHMEMPAGES)

#undef EM
#undef EMe

#define EM(a)	TRACE_DEFINE_ENUM(a);
#define EMe(a)	TRACE_DEFINE_ENUM(a);

TRACE_MM_PAGES

#undef EM
#undef EMe

#define EM(a)	{ a, #a },
#define EMe(a)	{ a, #a }

TRACE_EVENT(rss_stat,

	TP_PROTO(struct mm_struct *mm,
		int member),

	TP_ARGS(mm, member),

	TP_STRUCT__entry(
		__field(unsigned int, mm_id)
		__field(unsigned int, curr)
		__field(int, member)
		__field(long, size)
	),

	TP_fast_assign(
		__entry->mm_id = mm_ptr_to_hash(mm);
		/*
		 * curr is true if the mm matches the current task's mm_struct.
		 * Since kthreads (PF_KTHREAD) have no mm_struct of their own
		 * but can borrow one via kthread_use_mm(), we must filter them
		 * out to avoid incorrectly attributing the RSS update to them.
		 */
		__entry->curr = current->mm == mm && !(current->flags & PF_KTHREAD);
		__entry->member = member;
		__entry->size = (percpu_counter_sum_positive(&mm->rss_stat[member])
							    << PAGE_SHIFT);
	),

	TP_printk("mm_id=%u curr=%d type=%s size=%ldB",
		__entry->mm_id,
		__entry->curr,
		__print_symbolic(__entry->member, TRACE_MM_PAGES),
		__entry->size)
	);
#endif /* _TRACE_KMEM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
