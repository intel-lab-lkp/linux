/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FREETYPE_H
#define _LINUX_FREETYPE_H

#include <linux/types.h>

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

#endif /* _LINUX_FREETYPE_H */
