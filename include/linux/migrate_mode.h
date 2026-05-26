/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MIGRATE_MODE_H_INCLUDED
#define MIGRATE_MODE_H_INCLUDED
/*
 * MIGRATE_ASYNC means never block
 * MIGRATE_ASYNC_NON_TEMPORAL_STORES means never block and use non-temporal
 * stores if supported by the architecture
 * MIGRATE_SYNC_LIGHT in the current implementation means to allow blocking
 *	on most operations but not ->writepage as the potential stall time
 *	is too significant
 * MIGRATE_SYNC will block when migrating pages
 */
enum migrate_mode {
	MIGRATE_ASYNC,
	MIGRATE_ASYNC_NON_TEMPORAL_STORES,
	MIGRATE_SYNC_LIGHT,
	MIGRATE_SYNC,
};

static inline bool migrate_mode_is_async(enum migrate_mode mode)
{
	return mode == MIGRATE_ASYNC ||
		mode == MIGRATE_ASYNC_NON_TEMPORAL_STORES;
}

enum migrate_reason {
	MR_COMPACTION,
	MR_MEMORY_FAILURE,
	MR_MEMORY_HOTPLUG,
	MR_SYSCALL,		/* also applies to cpusets */
	MR_MEMPOLICY_MBIND,
	MR_NUMA_MISPLACED,
	MR_CONTIG_RANGE,
	MR_LONGTERM_PIN,
	MR_DEMOTION,
	MR_DAMON,
	MR_TYPES
};

#endif		/* MIGRATE_MODE_H_INCLUDED */
