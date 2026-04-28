/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Functions to handle the cached directory entries
 *
 *  Copyright (c) 2022, Ronnie Sahlberg <lsahlber@redhat.com>
 */

#ifndef _CACHED_DIR_H
#define _CACHED_DIR_H

#include <linux/completion.h>
#include <linux/build_bug.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/netfs.h>

struct cifs_search_info;

/* Timeout for waiting on async dcache population to complete */
#define CIFS_DCACHE_WAIT_TIMEOUT	(HZ / 10)

#define CIFS_CACHED_INLINE_NAME_LEN	64
#define CIFS_CACHED_DIRENT_LIST_THRESHOLD	64

struct cached_dirent {
	const char *name;
	u32 name_len;
	bool external_name;
	bool tombstone;
	u32 inline_name_off;
	loff_t ctx_pos;
	struct cifs_fattr fattr;
};

/*
 * Folio-backed cached directory entry storage:
 *
 * Directory entries are stored in a folio_queue managed by cached_dirents.
 * Each folio's virtual address points to a cifs_cached_dir_mapping structure,
 * which combines directory metadata and a variable-length array of cached_dirent
 * entries in a single folio allocation.
 *
 * Layout within each folio:
 *   [cifs_cached_dir_mapping] [cached_dirent[0]] ... [cached_dirent[n]]
 *                             ^                                            ^
 *                             |-------- entries_count ---------|
 *                             |-------- name_tail_offset (growing downward) ---------|
 *                             Inline name data (packed at tail of the folio)
 *
 * Field meanings:
 *   name_tail_offset: Current start offset of inline-name storage in the folio.
 *                     This moves downward as inline names are packed from tail.
 *   folio_full: Set when this folio cannot accept another cached_dirent record
 *               (record array would collide with inline-name tail region).
 *   folio_is_eof: Set when this folio contains the last emitted dirent for the
 *                 cached directory stream; readers stop when this folio is seen.
 *
 * Inline name optimization:
 *   Names <= CIFS_CACHED_INLINE_NAME_LEN are packed at the tail of the folio,
 *   after the last dirent entry. This avoids per-name allocation. For longer names,
 *   external_name is set and a separate kstrndup'd pointer is used.
 *
 * Tracking and lookup:
 *   A hash table (lookup_ht) in cached_dirents indexes all entries by name.
 *   Each hash entry (cached_dir_lookup_entry) records:
 *     - name pointer (points into inline region or external memory)
 *     - dirent pointer (points to cached_dirent in folio or list allocation)
 *   This enables O(1) lookups during dirent reservation and update operations,
 *   while also allowing list-backed staging to reuse cached_dirent directly.
 *
 * Sequencing and position tracking:
 *   last_pos tracks the directory position (ctx->pos) of the last entry added
 *   to this folio. When adding the next entry, we use last_pos + 1 to maintain
 *   consistent incrementing positions used for directory iteration.
 */
struct cifs_cached_dir_mapping {
	u64 last_cookie;
	u32 entries_count;
	u32 name_tail_offset;
	u32 folio_full:1;
	u32 folio_is_eof:1;
	struct cached_dirent entries[];
};

struct cached_dir_lookup_entry {
	struct hlist_node hash_node;
	struct list_head list_node;
	struct completion dcache_complete;
	struct cached_dirent *dirent;
	u32 name_hash;
	bool pending_dcache;
};

/*
 * Per-directory dirent cache using a two-mode storage strategy:
 *
 * Small directories (up to CIFS_CACHED_DIRENT_LIST_THRESHOLD entries):
 *   Entries are stored as individually allocated cached_dirent structs linked
 *   via cached_dir_lookup_entry nodes in entry_list. Each entry carries its
 *   own name allocation. This avoids folio overhead for short-lived or small
 *   directories.
 *
 * Large directories (above the threshold):
 *   The list is converted to folio-backed storage. Entries are packed into
 *   folios managed by folioq, with names <= CIFS_CACHED_INLINE_NAME_LEN stored
 *   inline at the tail of each folio to reduce per-name allocations. A hash
 *   table (lookup_ht) provides O(1) name lookup in this mode.
 *
 * The active mode is determined by whether folioq is non-NULL. All CRUD
 * operations (insert, lookup, update, invalidate, release) dispatch to the
 * appropriate list or folioq implementation via mode-dispatching helpers.
 */
struct cached_dirents {
	bool is_valid:1;
	bool is_failed:1;
	struct file *file; /*
			    * Used to associate the cache with a single
			    * open file instance.
			    */
	struct inode *dir_inode;
	struct mutex de_mutex;
	loff_t pos;		 /* Expected ctx->pos */
	struct folio_queue *folioq;
	struct list_head entry_list;
	unsigned int entry_list_count;
	/*
	 * Insertion cursor used by add_cached_dirent() to avoid rescanning folioq
	 * from the head on every append.
	 */
	struct folio_queue *insert_cursor_fq;
	unsigned int insert_cursor_slot;
	unsigned int insert_cursor_folio_index;
	size_t folioq_size;
	unsigned long external_name_bytes;
	struct hlist_head *lookup_ht;
	unsigned long lookup_bytes;
	/* accounting for cached entries in this directory */
	unsigned long entries_count;
	unsigned long bytes_used;
};

struct cached_fid {
	struct rb_node node;
	struct hlist_node dentry_node;
	struct list_head dying_entry;
	struct cached_fids *cfids;
	const char *path;
	bool has_lease;
	bool is_open;
	bool on_list;
	bool file_all_info_is_valid;
	unsigned long time; /* jiffies of when lease was taken */
	unsigned long last_access_time; /* jiffies of when last accessed */
	struct kref refcount;
	struct cifs_fid fid;
	struct cifs_tcon *tcon;
	struct dentry *dentry;
	struct work_struct put_work;
	struct work_struct close_work;
	struct cached_dirents dirents;
	struct mutex cfid_open_mutex; /* Serializes OPEN response processing and lease key population */
	spinlock_t cfid_lock; /* Protects: has_lease, time, is_open, file_all_info_is_valid, last_access_time, fid.lease_key reads */

	/* Must be last as it ends in a flexible-array member. */
	struct smb2_file_all_info file_all_info;
};

struct cached_dirent_lookup_result {
	bool found;
	bool under_active_lease;
	bool fully_populated;
	struct cifs_fattr fattr;
};

/* default MAX_CACHED_FIDS is 16 */
struct cached_fids {
	/* Must be held when:
	 * - modifying cfids->entries list (add/remove entries)
	 * - modifying cfids->dying list
	 * - modifying cfid->on_list or cfids->num_entries
	 *
	 * Lock ordering: if you need both cfid_list_lock and cfid_lock,
	 * acquire cfid_list_lock FIRST, then cfid_lock to avoid deadlock.
	 */
	spinlock_t cfid_list_lock;
	int num_entries;
	struct rb_root entries;
	struct hlist_head *dentry_ht;
	struct list_head dying;
	struct delayed_work laundromat_work;
	/* aggregate accounting for all cached dirents under this tcon */
	atomic_long_t total_dirents_entries;
	atomic64_t total_dirents_bytes;
};

/* Module-wide directory cache accounting (defined in cifsfs.c) */
extern atomic64_t cifs_dircache_bytes_used; /* bytes across all mounts */

static inline bool
is_valid_cached_dir(struct cached_fid *cfid)
{
	return cfid->time && cfid->has_lease;
}

bool cached_dir_is_valid(struct cached_fid *cfid);
bool cached_dir_copy_lease_key(struct cached_fid *cfid,
			      __u8 lease_key[SMB2_LEASE_KEY_SIZE]);

struct cached_fids *init_cached_dirs(void);
void free_cached_dirs(struct cached_fids *cfids);
int open_cached_dir(unsigned int xid, struct cifs_tcon *tcon, const char *path,
		    struct cifs_sb_info *cifs_sb, bool lookup_only,
		    struct cached_fid **ret_cfid);
int open_cached_dir_by_dentry(struct cifs_tcon *tcon, struct dentry *dentry,
			      struct cached_fid **ret_cfid);
void close_cached_dir(struct cached_fid *cfid);
void cifs_set_srch_inf_cfid(struct cifs_search_info *srch_inf,
			   struct cached_fid *cfid);
void cifs_put_srch_inf_cfid(struct cifs_search_info *srch_inf);
bool emit_cached_dir_if_valid(struct cached_fid *cfid,
			      struct file *file,
			      struct dir_context *ctx);
bool add_to_cached_dir(struct cached_fid *cfid,
		       struct dir_context *ctx,
		       const char *name,
		       int namelen,
		       struct cifs_fattr *fattr,
		       struct file *file);
void update_pos_cached_dir(struct cached_fid *cfid,
				      struct file *file);
void complete_cached_dir(struct cached_fid *cfid,
					struct dir_context *ctx,
					struct file *file);
int lookup_cached_dir(struct cached_fid *cfid,
				 const char *name, unsigned int namelen,
				 struct cached_dirent_lookup_result *result);
void invalidate_cached_dir_contents(struct cached_fid *cfid);
bool update_dirent_in_cached_dir(struct cached_fid *cfid,
				  const char *name,
				  unsigned int namelen,
				  const struct cifs_fattr *fattr);
bool invalidate_dirent_in_cached_dir(struct cached_fid *cfid,
				      const char *name,
				      unsigned int namelen);
void cifs_complete_pending_dcache(struct cached_fid *cfid,
				  const char *name, unsigned int namelen);
int cifs_wait_for_pending_dcache(struct cached_fid *cfid,
				 const char *name, unsigned int namelen);
void drop_cached_dir_by_name(const unsigned int xid, struct cifs_tcon *tcon,
			     const char *name, struct cifs_sb_info *cifs_sb);
void close_all_cached_dirs(struct cifs_sb_info *cifs_sb);
void invalidate_all_cached_dirs_nowait(struct cifs_tcon *tcon);
void invalidate_all_cached_dirs(struct cifs_tcon *tcon);
bool cached_dir_lease_break(struct cifs_tcon *tcon, __u8 lease_key[16]);

#endif			/* _CACHED_DIR_H */
