/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FREETYPE_H
#define _LINUX_FREETYPE_H

#include <linux/types.h>
#include <linux/mmdebug.h>

/*
 * A migratetype is the part of a freetype that encodes the mobility
 * requirements for the allocations the freelist is intended to serve.
 *
 * It's also currently overloaded to encode page isolation state.
 */
enum migratetype {
	MIGRATE_UNMOVABLE,
	MIGRATE_MOVABLE,
	MIGRATE_RECLAIMABLE,
	MIGRATE_PCPTYPES,	/* the number of types on the pcp lists */
	MIGRATE_HIGHATOMIC = MIGRATE_PCPTYPES,
#ifdef CONFIG_CMA
	/*
	 * MIGRATE_CMA migration type is designed to mimic the way
	 * ZONE_MOVABLE works.  Only movable pages can be allocated
	 * from MIGRATE_CMA pageblocks and page allocator never
	 * implicitly change migration type of MIGRATE_CMA pageblock.
	 *
	 * The way to use it is to change migratetype of a range of
	 * pageblocks to MIGRATE_CMA which can be done by
	 * __free_pageblock_cma() function.
	 */
	MIGRATE_CMA,
	__MIGRATE_TYPE_END = MIGRATE_CMA,
#else
	__MIGRATE_TYPE_END = MIGRATE_HIGHATOMIC,
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	MIGRATE_ISOLATE,	/* can't allocate from here */
#endif
	MIGRATE_TYPES
};

/* In mm/page_alloc.c; keep in sync also with show_migration_types() there */
extern const char * const migratetype_names[MIGRATE_TYPES];

#ifdef CONFIG_CMA
#  define is_migrate_cma(migratetype) unlikely((migratetype) == MIGRATE_CMA)
#else
#  define is_migrate_cma(migratetype) false
#endif

static inline bool is_migrate_movable(int mt)
{
	return is_migrate_cma(mt) || mt == MIGRATE_MOVABLE;
}

/*
 * Check whether a migratetype can be merged with another migratetype.
 *
 * It is only mergeable when it can fall back to other migratetypes for
 * allocation. See fallbacks[MIGRATE_TYPES][3] in page_alloc.c.
 */
static inline bool migratetype_is_mergeable(int mt)
{
	return mt < MIGRATE_PCPTYPES;
}

/*
 * A freetype is the index used to identify free lists. This consists of a
 * migratetype, and other bits which encode orthogonal properties of memory.
 */
typedef struct {
	int migratetype;
} freetype_t;

/*
 * Return a dense linear index for freetypes that have lists in the free area.
 * Return -1 for other freetypes.
 */
static inline int freetype_idx(freetype_t freetype)
{
	return freetype.migratetype;
}

/* No freetype flags actually exist yet. */
#define NR_FREETYPE_IDXS MIGRATE_TYPES

static inline unsigned int freetype_flags(freetype_t freetype)
{
	/* No flags supported yet. */
	return 0;
}

static inline bool freetypes_equal(freetype_t a, freetype_t b)
{
	return a.migratetype == b.migratetype;
}

static inline freetype_t migrate_to_freetype(enum migratetype mt,
					     unsigned int flags)
{
	freetype_t freetype;

	/* No flags supported yet. */
	VM_WARN_ON_ONCE(flags);

	freetype.migratetype = mt;
	return freetype;
}

static inline enum migratetype free_to_migratetype(freetype_t freetype)
{
	return freetype.migratetype;
}

/* Convenience helper, return the freetype modified to have the migratetype. */
static inline freetype_t freetype_with_migrate(freetype_t freetype,
					       enum migratetype migratetype)
{
	return migrate_to_freetype(migratetype, freetype_flags(freetype));
}

#endif /* _LINUX_FREETYPE_H */
