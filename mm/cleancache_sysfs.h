/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CLEANCACHE_SYSFS_H__
#define __CLEANCACHE_SYSFS_H__

enum cleancache_stat {
	STORED,
	SKIPPED,
	RESTORED,
	MISSED,
	RECLAIMED,
	RECALLED,
	INVALIDATED,
	CLEANCACHE_STAT_NR
};

enum cleancache_pool_stat {
	POOL_SIZE,
	POOL_CACHED,
	POOL_RECALLED,
	CLEANCACHE_POOL_STAT_NR
};

struct cleancache_pool_stats {
	struct kobject kobj;
	int pool_id;
	atomic64_t stats[CLEANCACHE_POOL_STAT_NR];
};

#ifdef CONFIG_CLEANCACHE_SYSFS
void cleancache_stat_inc(enum cleancache_stat type);
void cleancache_stat_add(enum cleancache_stat type, unsigned long delta);
void cleancache_pool_stat_inc(struct cleancache_pool_stats *pool_stats,
			 enum cleancache_pool_stat type);
void cleancache_pool_stat_dec(struct cleancache_pool_stats *pool_stats,
			 enum cleancache_pool_stat type);
void cleancache_pool_stat_add(struct cleancache_pool_stats *pool_stats,
			 enum cleancache_pool_stat type, long delta);
struct cleancache_pool_stats *cleancache_create_pool_stats(int pool_id);
struct kobject * __init cleancache_sysfs_create_root(void);
int cleancache_sysfs_create_pool(struct kobject *root_kobj,
				 struct cleancache_pool_stats *pool_stats,
				 const char *name);

#else /* CONFIG_CLEANCACHE_SYSFS */
static inline void cleancache_stat_inc(enum cleancache_stat type) {}
static inline void cleancache_stat_add(enum cleancache_stat type, unsigned long delta) {}
static inline void cleancache_pool_stat_inc(struct cleancache_pool_stats *pool_stats,
				       enum cleancache_pool_stat type) {}
static inline void cleancache_pool_stat_dec(struct cleancache_pool_stats *pool_stats,
				       enum cleancache_pool_stat type) {}
static inline void cleancache_pool_stat_add(struct cleancache_pool_stats *pool_stats,
				       enum cleancache_pool_stat type, long delta) {}
static inline
struct cleancache_pool_stats *cleancache_create_pool_stats(int pool_id) { return NULL; }

#endif /* CONFIG_CLEANCACHE_SYSFS */

#endif /* __CLEANCACHE_SYSFS_H__ */
