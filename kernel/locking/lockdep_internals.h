/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel/lockdep_internals.h
 *
 * Runtime locking correctness validator
 *
 * lockdep subsystem internal functions and variables.
 */

#include <linux/types.h>
#include <linux/reciprocal_div.h>
#include <linux/log2.h>
#include <asm/barrier.h>

#define LOCKDEP_SLAB_SIZE	(64 * 1024)
#define LOCKDEP_MAX_SLABS	512
#define LOCKDEP_DEFAULT_SLABS	64

/*
 * Lock-class usage-state bits:
 */
enum lock_usage_bit {
#define LOCKDEP_STATE(__STATE)		\
	LOCK_USED_IN_##__STATE,		\
	LOCK_USED_IN_##__STATE##_READ,	\
	LOCK_ENABLED_##__STATE,		\
	LOCK_ENABLED_##__STATE##_READ,
#include "lockdep_states.h"
#undef LOCKDEP_STATE
	LOCK_USED,
	LOCK_USED_READ,
	LOCK_USAGE_STATES,
};

/* states after LOCK_USED_READ are not traced and printed */
static_assert(LOCK_TRACE_STATES == LOCK_USAGE_STATES);

#define LOCK_USAGE_READ_MASK 1
#define LOCK_USAGE_DIR_MASK  2
#define LOCK_USAGE_STATE_MASK (~(LOCK_USAGE_READ_MASK | LOCK_USAGE_DIR_MASK))

/*
 * Usage-state bitmasks:
 */
#define __LOCKF(__STATE)	LOCKF_##__STATE = (1 << LOCK_##__STATE),

enum {
#define LOCKDEP_STATE(__STATE)						\
	__LOCKF(USED_IN_##__STATE)					\
	__LOCKF(USED_IN_##__STATE##_READ)				\
	__LOCKF(ENABLED_##__STATE)					\
	__LOCKF(ENABLED_##__STATE##_READ)
#include "lockdep_states.h"
#undef LOCKDEP_STATE
	__LOCKF(USED)
	__LOCKF(USED_READ)
};

enum {
#define LOCKDEP_STATE(__STATE)	LOCKF_ENABLED_##__STATE |
	LOCKF_ENABLED_IRQ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_USED_IN_##__STATE |
	LOCKF_USED_IN_IRQ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_ENABLED_##__STATE##_READ |
	LOCKF_ENABLED_IRQ_READ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE

#define LOCKDEP_STATE(__STATE)	LOCKF_USED_IN_##__STATE##_READ |
	LOCKF_USED_IN_IRQ_READ =
#include "lockdep_states.h"
	0,
#undef LOCKDEP_STATE
};

#define LOCKF_ENABLED_IRQ_ALL (LOCKF_ENABLED_IRQ | LOCKF_ENABLED_IRQ_READ)
#define LOCKF_USED_IN_IRQ_ALL (LOCKF_USED_IN_IRQ | LOCKF_USED_IN_IRQ_READ)

#define LOCKF_IRQ (LOCKF_ENABLED_IRQ | LOCKF_USED_IN_IRQ)
#define LOCKF_IRQ_READ (LOCKF_ENABLED_IRQ_READ | LOCKF_USED_IN_IRQ_READ)

/*
 * CONFIG_LOCKDEP_SMALL is defined for sparc. Sparc requires .text,
 * .data and .bss to fit in required 32MB limit for the kernel. With
 * CONFIG_LOCKDEP we could go over this limit and cause system boot-up problems.
 * So, reduce the static allocations for lockdeps related structures so that
 * everything fits in current required size limit.
 */
#ifdef CONFIG_LOCKDEP_SMALL
/*
 * MAX_LOCKDEP_ENTRIES is the maximum number of lock dependencies
 * we track.
 *
 * We use the per-lock dependency maps in two ways: we grow it by adding
 * every to-be-taken lock to all currently held lock's own dependency
 * table (if it's not there yet), and we check it for lock order
 * conflicts and deadlocks.
 */
#define MAX_LOCKDEP_ENTRIES	16384UL
#define MAX_LOCKDEP_CHAINS_BITS	15
#define MAX_STACK_TRACE_ENTRIES	262144UL
#define STACK_TRACE_HASH_SIZE	8192
#else
#define MAX_LOCKDEP_ENTRIES	(1UL << CONFIG_LOCKDEP_BITS)

#define MAX_LOCKDEP_CHAINS_BITS	CONFIG_LOCKDEP_CHAINS_BITS

/*
 * Stack-trace: tightly packed array of stack backtrace
 * addresses. Protected by the hash_lock.
 */
#define MAX_STACK_TRACE_ENTRIES	(1UL << CONFIG_LOCKDEP_STACK_TRACE_BITS)
#define STACK_TRACE_HASH_SIZE	(1 << CONFIG_LOCKDEP_STACK_TRACE_HASH_BITS)
#endif

/*
 * Bit definitions for lock_chain.irq_context
 */
#define LOCK_CHAIN_SOFTIRQ_CONTEXT	(1 << 0)
#define LOCK_CHAIN_HARDIRQ_CONTEXT	(1 << 1)

#define MAX_LOCKDEP_CHAINS	(1UL << MAX_LOCKDEP_CHAINS_BITS)

#define AVG_LOCKDEP_CHAIN_DEPTH		5
#define MAX_LOCKDEP_CHAIN_HLOCKS (MAX_LOCKDEP_CHAINS * AVG_LOCKDEP_CHAIN_DEPTH)

/*
 * Compile-time precomputation of struct reciprocal_value using the canonical
 * Granlund-Montgomery algorithm matching lib/math/reciprocal_div.c.
 */
#define RECIPROCAL_VALUE_INIT(d) { \
	.m = (u32)((((1ULL << 32) * ((1ULL << (ilog2((d) - 1) + 1)) - (d))) / (d)) + 1), \
	.sh1 = (ilog2((d) - 1) + 1) > 0 ? 1 : 0, \
	.sh2 = (ilog2((d) - 1) + 1) > 1 ? (ilog2((d) - 1) + 1) - 1 : 0, \
}

/*
 * Chunked Array Tables:
 * Replaces flat monolithic BSS arrays with 2D chunk pointer matrices.
 * Chunk 0 is statically allocated in BSS for early boot, while subsequent
 * chunks are claimed from the memblock reservoir via lockdep_claim_slab().
 * Indexing uses a compile-time hybrid: single-cycle bit shifts for power-of-2
 * elements, and Granlund-Montgomery reciprocal divide for non-power-of-2.
 */
#define DECLARE_CHUNKED_ARRAY(name, type)					\
	enum {									\
		name##_PER_CHUNK = (LOCKDEP_SLAB_SIZE / sizeof(type)),		\
		name##_IS_P2     = (!(name##_PER_CHUNK & (name##_PER_CHUNK - 1))), \
		name##_SHIFT     = (__builtin_ctz(name##_PER_CHUNK)),		\
		name##_MASK      = (name##_PER_CHUNK - 1),			\
	};									\
	extern type * name##_chunks[LOCKDEP_MAX_SLABS];				\
	extern const struct reciprocal_value name##_rv;				\
	static __always_inline type *idx_to_##name(unsigned int idx)		\
	{									\
		unsigned int chunk, offset;					\
		type *chunk_ptr;						\
										\
		if (name##_IS_P2) {						\
			chunk = idx >> name##_SHIFT;				\
			offset = idx & name##_MASK;				\
		} else {							\
			chunk = reciprocal_divide(idx, name##_rv);		\
			offset = idx - (chunk * name##_PER_CHUNK);		\
		}								\
										\
		if (unlikely(chunk >= LOCKDEP_MAX_SLABS))			\
			return NULL;						\
		/* Pairs with smp_store_release() when new chunk slabs are published */ \
		chunk_ptr = smp_load_acquire(&name##_chunks[chunk]);		\
		if (unlikely(!chunk_ptr))					\
			return NULL;						\
		return &chunk_ptr[offset];					\
	}

#define DEFINE_CHUNKED_ARRAY(name, type)						\
	static type name##_chunk0[name##_PER_CHUNK];				\
	type *name##_chunks[LOCKDEP_MAX_SLABS] = { name##_chunk0 };		\
	static unsigned int nr_##name##_chunks = 1;				\
	const struct reciprocal_value name##_rv =				\
		RECIPROCAL_VALUE_INIT(name##_PER_CHUNK)

DECLARE_CHUNKED_ARRAY(lock_chain, struct lock_chain);
DECLARE_CHUNKED_ARRAY(chain_hlock, u16);
void lockdep_chain_stats(unsigned int *nr_chunks, size_t *chunk_size, size_t *tail_used);

#define LOCK_USAGE_CHARS (2*XXX_LOCK_USAGE_STATES + 1)

extern void get_usage_chars(struct lock_class *class,
			    char usage[LOCK_USAGE_CHARS]);

extern const char *__get_key_name(const struct lockdep_subclass_key *key,
				  char *str);

struct lock_class *lock_chain_get_class(struct lock_chain *chain, int i);

extern unsigned long nr_lock_classes;
extern unsigned long nr_zapped_classes;
extern unsigned long nr_zapped_lock_chains;
extern unsigned long nr_list_entries;
extern unsigned long nr_dynamic_keys;
long lockdep_next_lockchain(long i);
unsigned long lock_chain_count(void);
extern unsigned long nr_stack_trace_entries;

extern unsigned int nr_hardirq_chains;
extern unsigned int nr_softirq_chains;
extern unsigned int nr_process_chains;
extern unsigned int nr_free_chain_hlocks;
extern unsigned int nr_lost_chain_hlocks;
extern unsigned int nr_large_chain_blocks;

extern unsigned int max_lockdep_depth;
extern unsigned int max_bfs_queue_depth;
extern unsigned long max_lock_class_idx;

DECLARE_2D_RADIX(lock_class, struct lock_class);
extern unsigned long lock_classes_in_use[];

struct lockdep_slab_usage {
	unsigned int lock_classes;
	unsigned int direct_deps;
	unsigned int lock_chains;
	unsigned int chain_hlocks;
	unsigned int stack_traces;
};

struct lockdep_slab_stats {
	unsigned int total_slabs;
	unsigned int used_slabs;
	struct lockdep_slab_usage usage;
};

unsigned int chain_hlocks_used(void);
unsigned long lock_chain_count(void);
void lockdep_get_slab_stats(struct lockdep_slab_stats *st);

#ifdef CONFIG_PROVE_LOCKING
extern unsigned long lockdep_count_forward_deps(struct lock_class *);
extern unsigned long lockdep_count_backward_deps(struct lock_class *);
#ifdef CONFIG_TRACE_IRQFLAGS
u64 lockdep_stack_trace_count(void);
u64 lockdep_stack_hash_count(void);
#endif
#else
static inline unsigned long
lockdep_count_forward_deps(struct lock_class *class)
{
	return 0;
}
static inline unsigned long
lockdep_count_backward_deps(struct lock_class *class)
{
	return 0;
}
#endif

#ifdef CONFIG_DEBUG_LOCKDEP

#include <asm/local.h>
/*
 * Various lockdep statistics.
 * We want them per cpu as they are often accessed in fast path
 * and we want to avoid too much cache bouncing.
 */
struct lockdep_stats {
	unsigned long  chain_lookup_hits;
	unsigned int   chain_lookup_misses;
	unsigned long  hardirqs_on_events;
	unsigned long  hardirqs_off_events;
	unsigned long  redundant_hardirqs_on;
	unsigned long  redundant_hardirqs_off;
	unsigned long  softirqs_on_events;
	unsigned long  softirqs_off_events;
	unsigned long  redundant_softirqs_on;
	unsigned long  redundant_softirqs_off;
	int            nr_unused_locks;
	unsigned int   nr_redundant_checks;
	unsigned int   nr_redundant;
	unsigned int   nr_cyclic_checks;
	unsigned int   nr_find_usage_forwards_checks;
	unsigned int   nr_find_usage_backwards_checks;

	/*
	 * Per lock class locking operation stat counts
	 */
	unsigned long lock_class_ops[MAX_LOCKDEP_KEYS];
};

DECLARE_PER_CPU(struct lockdep_stats, lockdep_stats);

#define __debug_atomic_inc(ptr)					\
	this_cpu_inc(lockdep_stats.ptr);

#define debug_atomic_inc(ptr)			{		\
	WARN_ON_ONCE(!irqs_disabled());				\
	__this_cpu_inc(lockdep_stats.ptr);			\
}

#define debug_atomic_dec(ptr)			{		\
	WARN_ON_ONCE(!irqs_disabled());				\
	__this_cpu_dec(lockdep_stats.ptr);			\
}

#define debug_atomic_read(ptr)		({				\
	struct lockdep_stats *__cpu_lockdep_stats;			\
	unsigned long long __total = 0;					\
	int __cpu;							\
	for_each_possible_cpu(__cpu) {					\
		__cpu_lockdep_stats = &per_cpu(lockdep_stats, __cpu);	\
		__total += __cpu_lockdep_stats->ptr;			\
	}								\
	__total;							\
})

static inline void debug_class_ops_inc(struct lock_class *class)
{
	int idx;

	idx = class->class_idx;
	__debug_atomic_inc(lock_class_ops[idx]);
}

static inline unsigned long debug_class_ops_read(struct lock_class *class)
{
	int idx, cpu;
	unsigned long ops = 0;

	idx = class->class_idx;
	for_each_possible_cpu(cpu)
		ops += per_cpu(lockdep_stats.lock_class_ops[idx], cpu);
	return ops;
}

#else
# define __debug_atomic_inc(ptr)	do { } while (0)
# define debug_atomic_inc(ptr)		do { } while (0)
# define debug_atomic_dec(ptr)		do { } while (0)
# define debug_atomic_read(ptr)		0
# define debug_class_ops_inc(ptr)	do { } while (0)
#endif
