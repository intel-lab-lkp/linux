/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLEANCACHE_H
#define _LINUX_CLEANCACHE_H

#include <linux/fs.h>
#include <linux/exportfs.h>
#include <linux/mm.h>

/* super_block->cleancache_id value for an invalid ID */
#define CLEANCACHE_ID_INVALID	-1

#define CLEANCACHE_KEY_MAX	6

/*
 * Cleancache requires every file with a folio in cleancache to have a
 * unique key unless/until the file is removed/truncated.  For some
 * filesystems, the inode number is unique, but for "modern" filesystems
 * an exportable filehandle is required (see exportfs.h)
 */
struct cleancache_filekey {
	union {
		ino_t ino;
		__u32 fh[CLEANCACHE_KEY_MAX];
		u32 key[CLEANCACHE_KEY_MAX];
	} u;
};

#ifdef CONFIG_CLEANCACHE

/* Hooks into MM and FS */
struct cleancache_filekey *cleancache_get_key(struct inode *inode,
					      struct cleancache_filekey *key);
void cleancache_add_fs(struct super_block *sb);
void cleancache_remove_fs(struct super_block *sb);
void cleancache_store_folio(struct folio *folio,
			    struct cleancache_filekey *key);
bool cleancache_restore_folio(struct folio *folio,
			      struct cleancache_filekey *key);
void cleancache_invalidate_folio(struct address_space *mapping,
				 struct folio *folio,
				 struct cleancache_filekey *key);
void cleancache_invalidate_inode(struct address_space *mapping,
				 struct cleancache_filekey *key);

/*
 * Backend API
 *
 * Cleancache does not touch page reference. Page refcount should be 1 when
 * page is placed or returned into cleancache and pages obtained from
 * cleancache will also have their refcount at 1.
 */
int cleancache_register_backend(const char *name, struct list_head *folios);
int cleancache_backend_get_folio(int area_id, struct folio *folio);
int cleancache_backend_put_folio(int area_id, struct folio *folio);

#else /* CONFIG_CLEANCACHE */

static inline
struct cleancache_filekey *cleancache_get_key(struct inode *inode,
					      struct cleancache_filekey *key)
{
	return NULL;
}
static inline void cleancache_add_fs(struct super_block *sb) {}
static inline void cleancache_remove_fs(struct super_block *sb) {}
static inline void cleancache_store_folio(struct folio *folio,
					  struct cleancache_filekey *key) {}
static inline bool cleancache_restore_folio(struct folio *folio,
					    struct cleancache_filekey *key)
{
	return false;
}
static inline void cleancache_invalidate_folio(struct address_space *mapping,
					       struct folio *folio,
					       struct cleancache_filekey *key) {}
static inline void cleancache_invalidate_inode(struct address_space *mapping,
					       struct cleancache_filekey *key) {}

static inline int cleancache_register_backend(const char *name,
		struct list_head *folios) { return -EOPNOTSUPP; }
static inline int cleancache_backend_get_folio(int area_id,
		struct folio *folio) { return -EOPNOTSUPP; }
static inline int cleancache_backend_put_folio(int area_id,
		struct folio *folio) { return -EOPNOTSUPP; }

#endif /* CONFIG_CLEANCACHE */

#endif /* _LINUX_CLEANCACHE_H */
