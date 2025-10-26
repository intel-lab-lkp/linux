/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLEANCACHE_H
#define _LINUX_CLEANCACHE_H

#include <linux/fs.h>
#include <linux/exportfs.h>
#include <linux/mm.h>

/* super_block->cleancache_id value for an invalid ID */
#define CLEANCACHE_ID_INVALID	-1

#define CLEANCACHE_KEY_MAX	6


#ifdef CONFIG_CLEANCACHE

/* Hooks into MM and FS */
int cleancache_add_fs(struct super_block *sb);
void cleancache_remove_fs(struct super_block *sb);
bool cleancache_store_folio(struct inode *inode, struct folio *folio);
bool cleancache_restore_folio(struct inode *inode, struct folio *folio);
bool cleancache_invalidate_folio(struct inode *inode, struct folio *folio);
bool cleancache_invalidate_inode(struct inode *inode);

/*
 * Backend API
 *
 * Cleancache does not touch folio references. Folio refcount should be 1 when
 * folio is placed or returned into cleancache and folios obtained from
 * cleancache will also have their refcount at 1.
 */
int cleancache_backend_register_pool(const char *name);
int cleancache_backend_get_folio(int pool_id, struct folio *folio);
int cleancache_backend_put_folio(int pool_id, struct folio *folio);
int cleancache_backend_put_folios(int pool_id, struct list_head *folios);

#else /* CONFIG_CLEANCACHE */

static inline int cleancache_add_fs(struct super_block *sb)
		{ return -EOPNOTSUPP; }
static inline void cleancache_remove_fs(struct super_block *sb) {}
static inline bool cleancache_store_folio(struct inode *inode,
					  struct folio *folio)
		{ return false; }
static inline bool cleancache_restore_folio(struct inode *inode,
					    struct folio *folio)
		{ return false; }
static inline bool cleancache_invalidate_folio(struct inode *inode,
					       struct folio *folio)
		{ return false; }
static inline bool cleancache_invalidate_inode(struct inode *inode)
		{ return false; }
static inline int cleancache_backend_register_pool(const char *name)
		{ return -EOPNOTSUPP; }
static inline int cleancache_backend_get_folio(int pool_id, struct folio *folio)
		{ return -EOPNOTSUPP; }
static inline int cleancache_backend_put_folio(int pool_id, struct folio *folio)
		{ return -EOPNOTSUPP; }
static inline int cleancache_backend_put_folios(int pool_id, struct list_head *folios)
		{ return -EOPNOTSUPP; }

#endif /* CONFIG_CLEANCACHE */

#endif /* _LINUX_CLEANCACHE_H */
