// SPDX-License-Identifier: GPL-2.0
/*
 *  Functions to handle the cached directory entries
 *
 *  Copyright (c) 2022, Ronnie Sahlberg <lsahlber@redhat.com>
 */

#include <linux/namei.h>
#include <linux/completion.h>
#include <linux/kmemleak.h>
#include <linux/hash.h>
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "smb2proto.h"
#include "cached_dir.h"
#include "trace.h"

static struct cached_fid *init_cached_dir(const char *path);
static void free_cached_dir(struct cached_fid *cfid);
static void smb2_close_cached_fid(struct kref *ref);
static void cfids_laundromat_worker(struct work_struct *work);

#define CACHED_DIRENT_HASH_BITS	7

struct cached_dir_dentry {
	struct list_head entry;
	struct dentry *dentry;
};

/* Generic helpers */
bool cached_dir_is_valid(struct cached_fid *cfid)
{
	bool valid;

	if (!cfid)
		return false;

	spin_lock(&cfid->cfid_lock);
	valid = is_valid_cached_dir(cfid);
	spin_unlock(&cfid->cfid_lock);

	return valid;
}

bool cached_dir_copy_lease_key(struct cached_fid *cfid,
			      __u8 lease_key[SMB2_LEASE_KEY_SIZE])
{
	bool valid;

	if (!cfid)
		return false;

	spin_lock(&cfid->cfid_lock);
	valid = is_valid_cached_dir(cfid);
	if (valid)
		memcpy(lease_key, cfid->fid.lease_key, SMB2_LEASE_KEY_SIZE);
	spin_unlock(&cfid->cfid_lock);

	return valid;
}

/* Cached mapping helpers */
static inline const char *cached_dirent_name(const struct cifs_cached_dir_mapping *cached_mapping,
					     const struct cached_dirent *de)
{
	if (de->external_name)
		return de->name;

	return ((const char *)cached_mapping) + de->inline_name_off;
}

static inline struct cifs_cached_dir_mapping *cached_dir_mapping(struct folio *folio)
{
	return folio_address(folio);
}

static inline size_t cached_dirent_array_bytes(unsigned int entries)
{
	return struct_size((struct cifs_cached_dir_mapping *)NULL, entries, entries);
}

static inline bool cached_dirent_has_space_for_record(const struct cifs_cached_dir_mapping *cached_mapping,
						      size_t record_bytes)
{
	return cached_dirent_array_bytes(cached_mapping->entries_count + 1) + record_bytes <=
		cached_mapping->name_tail_offset;
}

/* for short names, try to place them inside the folio */
static bool cached_dirent_try_inline_name(struct folio *folio,
					  struct cifs_cached_dir_mapping *cached_mapping,
					  struct cached_dirent *de,
					  const char *name,
					  unsigned int namelen,
					  const char **stored_name)
{
	char *base;
	u32 tail;

	if (namelen > CIFS_CACHED_INLINE_NAME_LEN)
		return false;

	/* try to fit cached_dirent+name in the same folio (inline) */
	if (!cached_dirent_has_space_for_record(cached_mapping, namelen))
		return false;

	base = folio_address(folio);
	if (!base)
		return false;

	tail = cached_mapping->name_tail_offset - namelen;
	memcpy(base + tail, name, namelen);
	de->external_name = false;
	de->inline_name_off = tail;
	de->name = NULL;
	cached_mapping->name_tail_offset = tail;
	*stored_name = base + tail;
	return true;
}

static unsigned int cached_dir_folio_count(struct cached_dirents *cde)
{
	struct folio_queue *fq;
	unsigned int count = 0;

	for (fq = cde->folioq; fq; fq = fq->next) {
		count += folioq_count(fq);
	}

	return count;
}

/* insert cursor helpers to aid fast appends to cached_dir */
static void cached_dir_reset_insert_cursor_locked(struct cached_dirents *cde)
{
	cde->insert_cursor_fq = cde->folioq;
	cde->insert_cursor_slot = 0;
	cde->insert_cursor_folio_index = 0;
}

static void cached_dir_set_insert_cursor_locked(struct cached_dirents *cde,
						struct folio_queue *fq,
						unsigned int slot,
						unsigned int folio_index)
{
	cde->insert_cursor_fq = fq;
	cde->insert_cursor_slot = slot;
	cde->insert_cursor_folio_index = folio_index;
}

static bool cached_dirents_use_folioq_locked(struct cached_dirents *cde)
{
	return cde->folioq != NULL;
}

static void cached_dir_init_new_folios(struct cached_dirents *cde,
				       unsigned int old_folio_count)
{
	struct folio_queue *fq;
	unsigned int folio_index = 0;

	for (fq = cde->folioq; fq; fq = fq->next) {
		for (int s = 0; s < folioq_count(fq); s++, folio_index++) {
			struct folio *folio = folioq_folio(fq, s);
			void *base;

			if (folio_index < old_folio_count)
				continue;

			base = folio_address(folio);
			if (base) {
				memset(base, 0, folio_size(folio));
				cached_dir_mapping(folio)->name_tail_offset = folio_size(folio);
			}
		}
	}
}

/*
 * Expand the folioq backing store for a cached directory by one PAGE_SIZE.
 * Called by add_cached_dirent_folioq_locked() when no free slot is found in
 * the existing folios, and by convert_cached_dirents_list_to_folioq_locked()
 * when initializing folioq mode for the first time.
 *
 * After growing, newly added folios are zeroed and their name_tail_offset is
 * set to folio_size so that inline name packing starts from the tail.
 * The insert cursor must be reset by the caller after this returns.
 */
static int grow_cached_dirents_folioq_locked(struct cached_dirents *cde)
{
	unsigned int old_folio_count;
	size_t old_size, target_size;
	int rc;

	old_folio_count = cached_dir_folio_count(cde);
	old_size = cde->folioq_size;
	target_size = old_size + PAGE_SIZE;

	cifs_dbg(FYI,
		 "cached_dir folioq alloc: old_size=%zu target_size=%zu\n",
		 old_size, target_size);

	rc = netfs_alloc_folioq_buffer(NULL, &cde->folioq,
				      &cde->folioq_size,
				      target_size, GFP_NOFS);
	if (rc < 0)
		return rc;

	cached_dir_init_new_folios(cde, old_folio_count);

	return 0;
}

/* lookup cached_dirent by traversing the list */
static struct cached_dir_lookup_entry *lookup_cached_dirent_list_locked(struct cached_dirents *cde,
							 const char *name,
							 unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	u32 name_hash;

	name_hash = full_name_hash(NULL, name, namelen);

	list_for_each_entry(entry, &cde->entry_list, list_node) {
		if (entry->name_hash == name_hash &&
		    entry->dirent &&
		    entry->dirent->name_len == namelen &&
		    memcmp(entry->dirent->name, name, namelen) == 0)
			return entry;
	}

	return NULL;
}

/* lookup cached_dirent in folioq by using the hash table */
static struct cached_dir_lookup_entry *lookup_cached_dirent_locked(struct cached_dirents *cde,
								   const char *name,
								   unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	struct hlist_head *bucket;
	u32 name_hash;

	if (!cde->lookup_ht)
		return NULL;

	name_hash = full_name_hash(NULL, name, namelen);
	bucket = &cde->lookup_ht[hash_32(name_hash, CACHED_DIRENT_HASH_BITS)];

	hlist_for_each_entry(entry, bucket, hash_node) {
		if (entry->name_hash == name_hash &&
		    entry->dirent &&
		    entry->dirent->name_len == namelen &&
		    memcmp(entry->dirent->name, name, namelen) == 0)
			return entry;
	}

	return NULL;
}

/* lookup wrapper to decide if the entry is in list or folioq */
static struct cached_dir_lookup_entry *lookup_cached_dirent_entry_locked(struct cached_dirents *cde,
								  const char *name,
								  unsigned int namelen)
{
	if (cached_dirents_use_folioq_locked(cde))
		return lookup_cached_dirent_locked(cde, name, namelen);

	return lookup_cached_dirent_list_locked(cde, name, namelen);
}

/* lookup the last cached_dir_mapping in the folioq */
static struct cifs_cached_dir_mapping *last_cached_dir_mapping_locked(struct cached_dirents *cde)
{
	struct folio_queue *fq;
	unsigned int slot;
	struct cifs_cached_dir_mapping *last = NULL;

	lockdep_assert_held(&cde->de_mutex);

	if (!cde->folioq)
		return NULL;

	/* Fast path: the insert cursor tracks the most recent append location. */
	if (cde->insert_cursor_fq) {
		slot = cde->insert_cursor_slot;
		if (slot < folioq_count(cde->insert_cursor_fq)) {
			last = cached_dir_mapping(folioq_folio(cde->insert_cursor_fq, slot));
			if (last && last->entries_count)
				return last;
		}
	}

	for (fq = cde->folioq; fq; fq = fq->next) {
		for (int s = 0; s < folioq_count(fq); s++) {
			struct cifs_cached_dir_mapping *cached_mapping;

			cached_mapping = cached_dir_mapping(folioq_folio(fq, s));
			if (cached_mapping && cached_mapping->entries_count)
				last = cached_mapping;
		}
	}

	return last;
}

/* emit dirents from the cache, starting with the current position of ctx */
static bool emit_cached_dirents(struct cached_dirents *cde,
				struct dir_context *ctx)
{
	struct folio_queue *fq;
	bool rc;

	lockdep_assert_held(&cde->de_mutex);

	/* if folioq is empty, this is a small dir; dirents will be found in list */
	if (!cde->folioq) {
		struct cached_dir_lookup_entry *entry;

		list_for_each_entry(entry, &cde->entry_list, list_node) {
			struct cached_dirent *dirent = entry->dirent;

			if (dirent->tombstone)
				continue;
			if (ctx->pos > dirent->ctx_pos)
				continue;

			ctx->pos = dirent->ctx_pos;
			rc = dir_emit(ctx, dirent->name, dirent->name_len,
				      dirent->fattr.cf_uniqueid,
				      dirent->fattr.cf_dtype);
			if (!rc)
				return rc;
			ctx->pos++;
		}

		return cde->is_valid;
	}

	/* large dir; emit from folioq */
	for (fq = cde->folioq; fq; fq = fq->next) {
		for (int s = 0; s < folioq_count(fq); s++) {
			struct folio *folio = folioq_folio(fq, s);
			struct cifs_cached_dir_mapping *cached_mapping;

			cached_mapping = cached_dir_mapping(folio);
			if (!cached_mapping)
				return false;

			for (u32 i = 0; i < cached_mapping->entries_count; i++) {
				struct cached_dirent *dirent = &cached_mapping->entries[i];
				const char *name;

				if (dirent->tombstone)
					continue;

				name = cached_dirent_name(cached_mapping, dirent);

				/*
				 * Skip all early entries prior to the current lseek()
				 * position.
				 */
				if (ctx->pos > dirent->ctx_pos)
					continue;
				/*
				 * We recorded the current ->pos value for the dirent
				 * when we stored it in the cache.
				 * However, this sequence of ->pos values may have holes
				 * in it, for example dot-dirs returned from the server
				 * are suppressed.
				 * Handle this by forcing ctx->pos to be the same as the
				 * ->pos of the current dirent we emit from the cache.
				 * This means that when we emit these entries from the cache
				 * we now emit them with the same ->pos value as in the
				 * initial scan.
				 */
				ctx->pos = dirent->ctx_pos;
				rc = dir_emit(ctx, name, dirent->name_len,
					      dirent->fattr.cf_uniqueid,
					      dirent->fattr.cf_dtype);
				if (!rc)
					return rc;
				ctx->pos++;
			}

			if (cached_mapping->folio_is_eof)
				return true;
		}
	}
	return true;
}

/* release the lookup hashtable */
static void release_lookup_table_locked(struct cached_dirents *cde)
{
	int bucket;

	if (!cde->lookup_ht)
		return;

	for (bucket = 0; bucket < (1 << CACHED_DIRENT_HASH_BITS); bucket++) {
		struct cached_dir_lookup_entry *entry;
		struct hlist_node *tmp;

		hlist_for_each_entry_safe(entry, tmp, &cde->lookup_ht[bucket], hash_node) {
			hlist_del(&entry->hash_node);
			kfree(entry);
		}
	}

	kfree(cde->lookup_ht);
	cde->lookup_ht = NULL;
	cde->lookup_bytes = 0;
}

/* release all cached_dirents in list */
static void release_cached_dirents_list_locked(struct cached_dirents *cde)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dir_lookup_entry *tmp;

	list_for_each_entry_safe(entry, tmp, &cde->entry_list, list_node) {
		list_del(&entry->list_node);
		if (entry->dirent) {
			if (entry->dirent->external_name)
				kfree((void *)entry->dirent->name);
			kfree(entry->dirent);
		}
		kfree(entry);
	}

	cde->entry_list_count = 0;
}

/* release all cached_dirents in folioq */
static void release_cached_dirents_folioq_locked(struct cached_dirents *cde)
{
	struct folio_queue *fq;

	lockdep_assert_held(&cde->de_mutex);

	for (fq = cde->folioq; fq; fq = fq->next) {
		for (int s = 0; s < folioq_count(fq); s++) {
			struct folio *folio = folioq_folio(fq, s);
			struct cifs_cached_dir_mapping *cached_mapping;

			cached_mapping = cached_dir_mapping(folio);
			if (!cached_mapping)
				continue;

			for (u32 i = 0; i < cached_mapping->entries_count; i++)
				if (cached_mapping->entries[i].external_name)
					kfree((void *)cached_mapping->entries[i].name);
		}
	}

	if (cde->folioq) {
		cifs_dbg(FYI, "cached_dir folioq free: old_size=%zu target_size=%d\n",
			 cde->folioq_size, 0);
		netfs_free_folioq_buffer(cde->folioq);
		cde->folioq = NULL;
	}

	cde->folioq_size = 0;
}

/* release wrapper for cached_dirents */
static void release_cached_dirents_locked(struct cached_dirents *cde)
{
	lockdep_assert_held(&cde->de_mutex);

	if (cached_dirents_use_folioq_locked(cde))
		release_cached_dirents_folioq_locked(cde);
	else
		release_cached_dirents_list_locked(cde);

	release_lookup_table_locked(cde);

	cde->entries_count = 0;
	cde->external_name_bytes = 0;
	cde->lookup_bytes = 0;
	cde->bytes_used = 0;
	cde->dir_inode = NULL;
	cached_dir_reset_insert_cursor_locked(cde);
}

/* invalidate cached_dirents and release resources, but keep the cache structure for reuse */
static void fail_cached_dir_locked(struct cached_dirents *cde)
{
	cde->is_failed = 1;
	release_cached_dirents_locked(cde);
	/*
	 * Reset the file pointer so the next cifs_readdir from position 0
	 * can claim this slot and repopulate the cache.
	 */
	cde->file = NULL;
}

/* insert cached_dirent into lookup hashtable */
static int insert_cached_dir_lookup_locked(struct cached_dirents *cde,
					   const char *name,
					   unsigned int namelen,
					   struct cached_dirent *dirent,
					   bool pending_dcache)
{
	struct cached_dir_lookup_entry *entry;
	struct hlist_head *bucket;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->name_hash = full_name_hash(NULL, name, namelen);
	entry->dirent = dirent;
	entry->pending_dcache = pending_dcache;
	init_completion(&entry->dcache_complete);

	bucket = &cde->lookup_ht[hash_32(entry->name_hash, CACHED_DIRENT_HASH_BITS)];
	hlist_add_head(&entry->hash_node, bucket);
	cde->lookup_bytes += sizeof(*entry);
	return 0;
}

/* add cached_dirent to folioq */
static bool add_cached_dirent_folioq_locked(struct cached_dirents *cde,
					    loff_t ctx_pos,
					    const char *name,
					    unsigned int namelen,
					    const struct cifs_fattr *fattr,
					    bool pending_dcache)
{
	struct cached_dirent *de;
	struct cifs_cached_dir_mapping *cached_mapping = NULL;
	const char *stored_name;
	struct folio *target_folio = NULL;
	struct folio_queue *fq;
	unsigned int cur_folio;
	unsigned int start_slot;
	int rc;
	bool grew = false;

	if (!cde->lookup_ht) {
		cde->lookup_ht = kcalloc(1 << CACHED_DIRENT_HASH_BITS,
					 sizeof(*cde->lookup_ht), GFP_KERNEL);
		if (!cde->lookup_ht) {
			fail_cached_dir_locked(cde);
			return false;
		}
	}

	/* Grow phase: ensure folioq exists */
	if (!cde->folioq) {
		rc = grow_cached_dirents_folioq_locked(cde);
		if (rc < 0) {
			fail_cached_dir_locked(cde);
			return false;
		}
		cached_dir_reset_insert_cursor_locked(cde);
	}

	if (!cde->insert_cursor_fq)
		cached_dir_reset_insert_cursor_locked(cde);

retry_insert:
	/* Insertion phase: try to find space in current folios */
	de = NULL;
	fq = cde->insert_cursor_fq;
	start_slot = cde->insert_cursor_slot;
	cur_folio = cde->insert_cursor_folio_index;
	if (!fq) {
		fq = cde->folioq;
		start_slot = 0;
		cur_folio = 0;
	}

	for (; fq && !de; fq = fq->next) {
		for (int s = start_slot; s < folioq_count(fq) && !de; s++, cur_folio++) {
			struct folio *folio = folioq_folio(fq, s);

			cached_mapping = cached_dir_mapping(folio);
			if (!cached_mapping)
				continue;

			if (cached_mapping->folio_full)
				continue;

			if (cached_dirent_has_space_for_record(cached_mapping, 0)) {
				target_folio = folio;
				de = &cached_mapping->entries[cached_mapping->entries_count];
				cached_dir_set_insert_cursor_locked(cde, fq, s, cur_folio);
				break;
			}

			cached_mapping->folio_full = 1;
		}
		start_slot = 0;
	}

	/* If no space found and haven't grown yet, grow and retry once */
	if (!de && !grew) {
		rc = grow_cached_dirents_folioq_locked(cde);
		if (rc < 0) {
			fail_cached_dir_locked(cde);
			return false;
		}

		cached_dir_reset_insert_cursor_locked(cde);
		grew = true;
		goto retry_insert;
	}

	if (!de) {
		fail_cached_dir_locked(cde);
		return false;
	}

	memset(de, 0, sizeof(*de));
	de->name_len = namelen;
	de->ctx_pos = ctx_pos;
	memcpy(&de->fattr, fattr, sizeof(*fattr));
	stored_name = NULL;
	if (!cached_dirent_try_inline_name(target_folio, cached_mapping, de,
					      name, namelen, &stored_name)) {
		de->name = kstrndup(name, namelen, GFP_KERNEL);
		if (!de->name) {
			fail_cached_dir_locked(cde);
			return false;
		}
		kmemleak_not_leak((void *)de->name);
		de->external_name = true;
		cde->external_name_bytes += (size_t)namelen + 1;
		stored_name = de->name;
	} else {
		de->external_name = false;
	}
	de->name = stored_name;

	if (insert_cached_dir_lookup_locked(cde, stored_name, namelen,
				   de,
				   pending_dcache) < 0) {
		if (de->external_name)
			kfree((void *)de->name);
		memset(de, 0, sizeof(*de));
		fail_cached_dir_locked(cde);
		return false;
	}

	cached_mapping->entries_count++;
	cde->entries_count++;
	cde->bytes_used = cde->folioq_size + cde->external_name_bytes +
				  cde->lookup_bytes;
	return true;
}

/* add cached_dirent to list */
static bool add_cached_dirent_list_locked(struct cached_dirents *cde,
					  loff_t ctx_pos,
					  const char *name,
					  unsigned int namelen,
					  const struct cifs_fattr *fattr)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *de;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return false;

	de = kzalloc(sizeof(*de), GFP_KERNEL);
	if (!de) {
		kfree(entry);
		return false;
	}

	de->name = kstrndup(name, namelen, GFP_KERNEL);
	if (!de->name) {
		kfree(de);
		kfree(entry);
		return false;
	}

	de->name_len = namelen;
	de->external_name = true;
	de->ctx_pos = ctx_pos;
	memcpy(&de->fattr, fattr, sizeof(*fattr));

	entry->dirent = de;
	entry->name_hash = full_name_hash(NULL, name, namelen);
	entry->pending_dcache = false;
	list_add_tail(&entry->list_node, &cde->entry_list);

	cde->entry_list_count++;
	cde->entries_count++;
	cde->external_name_bytes += (size_t)namelen + 1;
	cde->bytes_used = cde->external_name_bytes +
			  cde->entry_list_count * (sizeof(*entry) + sizeof(*de));
	return true;
}

/* convert cached_dirents from list to folioq format, freeing list entries */
static int convert_cached_dirents_list_to_folioq_locked(struct cached_dirents *cde)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dir_lookup_entry *tmp;
	unsigned long restored_entries = 0;

	if (cde->folioq)
		return 0;

	release_lookup_table_locked(cde);
	cde->entries_count = 0;
	cde->external_name_bytes = 0;
	cde->lookup_bytes = 0;
	cde->bytes_used = 0;

	list_for_each_entry_safe(entry, tmp, &cde->entry_list, list_node) {
		if (!add_cached_dirent_folioq_locked(cde, entry->dirent->ctx_pos,
						   entry->dirent->name,
						   entry->dirent->name_len,
						   &entry->dirent->fattr, false)) {
			return -ENOMEM;
		}

		restored_entries++;
		list_del(&entry->list_node);
		kfree((void *)entry->dirent->name);
		kfree(entry->dirent);
		kfree(entry);
	}

	cde->entry_list_count = 0;
	cde->entries_count = restored_entries;
	cde->bytes_used = cde->folioq_size + cde->external_name_bytes +
			  cde->lookup_bytes;
	return 0;
}

/* add cached_dirent, deciding whether to put it in the list or folioq */
static bool add_cached_dirent(struct cached_dirents *cde,
			      struct dir_context *ctx, const char *name,
			      int namelen, struct cifs_fattr *fattr,
			      struct file *file)
{
	int rc;

	lockdep_assert_held(&cde->de_mutex);

	if (cde->file != file)
		return false;
	if (cde->is_valid || cde->is_failed)
		return false;
	if (ctx->pos != cde->pos) {
		fail_cached_dir_locked(cde);
		return false;
	}

	if (!cached_dirents_use_folioq_locked(cde)) {
		if (cde->entry_list_count < CIFS_CACHED_DIRENT_LIST_THRESHOLD)
			return add_cached_dirent_list_locked(cde, ctx->pos, name,
						     namelen, fattr);

		rc = convert_cached_dirents_list_to_folioq_locked(cde);
		if (rc < 0) {
			fail_cached_dir_locked(cde);
			return false;
		}
	}

	if (!add_cached_dirent_folioq_locked(cde, ctx->pos, name, namelen, fattr,
					     true)) {
		fail_cached_dir_locked(cde);
		return false;
	}

	return true;
}

/*
 * emit cached dirents for the current ctx position if the cache is valid.
 * If there is no ongoing population for this directory (ctx->pos == 0) then
 * make the ongoing readdir call responsible for populating the cache
 */
bool emit_cached_dir_if_valid(struct cached_fid *cfid,
			      struct file *file,
			      struct dir_context *ctx)
{
	if (!cfid)
		return false;

	mutex_lock(&cfid->dirents.de_mutex);
	/*
	 * If this was reading from the start of the directory
	 * we need to initialize scanning and storing the
	 * directory content.
	 */
	if (ctx->pos == 0 && cfid->dirents.file == NULL) {
		cfid->dirents.file = file;
		cfid->dirents.dir_inode = file_inode(file);
		cfid->dirents.pos = 2;
		cached_dir_reset_insert_cursor_locked(&cfid->dirents);
		/*
		 * A previous population attempt may have failed and left
		 * is_failed set.  Clear it now so add_cached_dirent() will
		 * accept new entries from this readdir pass.
		 */
		cfid->dirents.is_failed = 0;
	}

	if (!cfid->dirents.is_valid) {
		mutex_unlock(&cfid->dirents.de_mutex);
		return false;
	}

	if (dir_emit_dots(file, ctx))
		emit_cached_dirents(&cfid->dirents, ctx);

	mutex_unlock(&cfid->dirents.de_mutex);
	return true;
}

/* update the cached dir position during a readdir population pass */
static void update_cached_dirents_count(struct cached_dirents *cde,
					struct file *file)
{
	if (cde->file != file)
		return;
	if (cde->is_valid || cde->is_failed)
		return;

	cde->pos++;
}

/* mark the cached_dirents as valid if readdir population pass completed successfully */
static void finished_cached_dirents_count(struct cached_dirents *cde,
					  struct dir_context *ctx,
					  struct file *file)
{
	struct cifs_cached_dir_mapping *cached_mapping;

	if (cde->file != file)
		return;
	if (cde->is_valid || cde->is_failed)
		return;
	if (ctx->pos != cde->pos)
		return;

	cached_mapping = last_cached_dir_mapping_locked(cde);
	if (cached_mapping)
		cached_mapping->folio_is_eof = 1;

	cde->is_valid = 1;
}

/* update the cached_dirent for a given name in list */
static bool update_cached_dirent_list_locked(struct cached_dirents *cde,
						     const char *name,
						     unsigned int namelen,
						     const struct cifs_fattr *fattr)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *dirent;

	entry = lookup_cached_dirent_list_locked(cde, name, namelen);
	if (!entry)
		return false;

	dirent = entry->dirent;
	if (!dirent)
		return false;

	memcpy(&dirent->fattr, fattr, sizeof(dirent->fattr));
	dirent->tombstone = false;
	return true;
}

/* update the cached_dirent for a given name in folioq */
static bool update_cached_dirent_folioq_locked(struct cached_dirents *cde,
						       const char *name,
						       unsigned int namelen,
						       const struct cifs_fattr *fattr)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *dirent;

	entry = lookup_cached_dirent_locked(cde, name, namelen);
	if (!entry)
		return false;

	dirent = entry->dirent;
	if (!dirent)
		return false;

	memcpy(&dirent->fattr, fattr, sizeof(dirent->fattr));
	dirent->tombstone = false;
	return true;
}

/* update wrapper to decide if the entry is in list or folioq */
static bool update_cached_dirent_locked(struct cached_dirents *cde,
						const char *name,
						unsigned int namelen,
						const struct cifs_fattr *fattr)
{
	if (cached_dirents_use_folioq_locked(cde))
		return update_cached_dirent_folioq_locked(cde, name, namelen,
							  fattr);

	return update_cached_dirent_list_locked(cde, name, namelen,
							 fattr);
}

/* invalidate a cached_dirent by name in list */
static bool invalidate_cached_dirent_list_locked(struct cached_dirents *cde,
						 const char *name,
						 unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *dirent;

	entry = lookup_cached_dirent_list_locked(cde, name, namelen);
	if (!entry)
		return true;

	dirent = entry->dirent;
	if (!dirent)
		return true;

	dirent->tombstone = true;
	return true;
}

/* invalidate a cached_dirent by name in folioq */
static bool invalidate_cached_dirent_folioq_locked(struct cached_dirents *cde,
						   const char *name,
						   unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *dirent;

	entry = lookup_cached_dirent_locked(cde, name, namelen);
	if (!entry)
		return true;

	dirent = entry->dirent;
	if (!dirent)
		return false;

	dirent->tombstone = true;
	if (entry->pending_dcache) {
		entry->pending_dcache = false;
		complete_all(&entry->dcache_complete);
	}

	return true;
}

/* invalidate wrapper to decide if the entry is in list or folioq */
static bool invalidate_cached_dirent_locked(struct cached_dirents *cde,
						const char *name,
						unsigned int namelen)
{
	if (cached_dirents_use_folioq_locked(cde))
		return invalidate_cached_dirent_folioq_locked(cde, name,
							      namelen);

	return invalidate_cached_dirent_list_locked(cde, name, namelen);
}

/* append a dirent to the cached_dir */
bool add_to_cached_dir(struct cached_fid *cfid,
		       struct dir_context *ctx,
		       const char *name,
		       int namelen,
		       struct cifs_fattr *fattr,
		       struct file *file)
{
	unsigned long old_entries;
	unsigned long new_entries;
	u64 old_bytes;
	u64 new_bytes;
	long entry_diff;
	long long bytes_diff;
	bool added = false;

	if (!cfid)
		return false;

	mutex_lock(&cfid->dirents.de_mutex);
	old_entries = cfid->dirents.entries_count;
	old_bytes = cfid->dirents.bytes_used;
	added = add_cached_dirent(&cfid->dirents, ctx, name, namelen,
				  fattr, file);
	new_entries = cfid->dirents.entries_count;
	new_bytes = cfid->dirents.bytes_used;
	mutex_unlock(&cfid->dirents.de_mutex);

	entry_diff = (long)new_entries - (long)old_entries;
	bytes_diff = (long long)new_bytes - (long long)old_bytes;

	if (entry_diff > 0) {
		atomic_long_add(entry_diff, &cfid->cfids->total_dirents_entries);
	} else if (entry_diff < 0) {
		atomic_long_sub(-entry_diff, &cfid->cfids->total_dirents_entries);
	}

	if (bytes_diff > 0) {
		atomic64_add(bytes_diff, &cfid->cfids->total_dirents_bytes);
		atomic64_add(bytes_diff, &cifs_dircache_bytes_used);
	} else if (bytes_diff < 0) {
		atomic64_sub(-bytes_diff, &cfid->cfids->total_dirents_bytes);
		atomic64_sub(-bytes_diff, &cifs_dircache_bytes_used);
	}


	return added;
}

/* update the cached_dir position during a readdir population pass */
void update_pos_cached_dir(struct cached_fid *cfid,
				      struct file *file)
{
	if (!cfid)
		return;

	mutex_lock(&cfid->dirents.de_mutex);
	update_cached_dirents_count(&cfid->dirents, file);
	mutex_unlock(&cfid->dirents.de_mutex);
}

/* signal completion of cached_dir population after a readdir pass */
void complete_cached_dir(struct cached_fid *cfid,
					struct dir_context *ctx,
					struct file *file)
{
	struct cached_dirents *cde;

	if (!cfid)
		return;

	cde = &cfid->dirents;
	mutex_lock(&cfid->dirents.de_mutex);
	finished_cached_dirents_count(cde, ctx, file);
	mutex_unlock(&cfid->dirents.de_mutex);
}

/*
 * lookup a cached_dirent by name, returning -ENOENT if not found or if the
 * entry is a tombstone.  The result struct is filled in with the fattr of the
 * found entry, and flags indicating whether the entry was found, whether the
 * cache was fully populated at the time of lookup, and whether there was an
 * active lease on the directory at the time of lookup.
 */
int lookup_cached_dir(struct cached_fid *cfid,
				 const char *name,
				 unsigned int namelen,
				 struct cached_dirent_lookup_result *result)
{
	struct cached_dir_lookup_entry *entry;
	struct cached_dirent *dirent;
	bool lease_active;

	if (!cfid || !name || !namelen || !result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));

	spin_lock(&cfid->cfid_lock);
	lease_active = is_valid_cached_dir(cfid);
	spin_unlock(&cfid->cfid_lock);

	mutex_lock(&cfid->dirents.de_mutex);
	result->under_active_lease = lease_active;
	result->fully_populated = cfid->dirents.is_valid;

	entry = lookup_cached_dirent_entry_locked(&cfid->dirents, name, namelen);
	if (!entry || !entry->dirent) {
		mutex_unlock(&cfid->dirents.de_mutex);
		return -ENOENT;
	}

	dirent = entry->dirent;
	if (dirent->tombstone) {
		mutex_unlock(&cfid->dirents.de_mutex);
		return -ENOENT;
	}

	result->found = true;
	memcpy(&result->fattr, &dirent->fattr, sizeof(result->fattr));

	mutex_unlock(&cfid->dirents.de_mutex);
	return 0;
}

/*
 * Invalidate all cached_dirents for a cached_fid. We generally
 * try to invalidate specific entries by name. This is used as
 * a last resort when we can't invalidate specific entries
 */
void invalidate_cached_dir_contents(struct cached_fid *cfid)
{
	if (!cfid)
		return;

	mutex_lock(&cfid->dirents.de_mutex);
	fail_cached_dir_locked(&cfid->dirents);
	mutex_unlock(&cfid->dirents.de_mutex);
}

/*
 * Update a cached_dirent for a given name.  Returns true if the entry was
 * found and updated, false if the entry was not found or if the cache is not
 * valid.
 */
bool update_dirent_in_cached_dir(struct cached_fid *cfid,
				  const char *name,
				  unsigned int namelen,
				  const struct cifs_fattr *fattr)
{
	bool updated = false;

	if (!cfid || !name || !namelen || !fattr)
		return false;

	mutex_lock(&cfid->dirents.de_mutex);
	updated = update_cached_dirent_locked(&cfid->dirents, name,
						      namelen, fattr);
	mutex_unlock(&cfid->dirents.de_mutex);
	return updated;
}

/*
 * Invalidate a cached_dirent for a given name.  Returns true if the entry was
 * found and invalidated, false if the entry was not found or if the cache is
 * not valid.
 */
bool invalidate_dirent_in_cached_dir(struct cached_fid *cfid,
				      const char *name,
				      unsigned int namelen)
{
	bool invalidated = false;

	if (!cfid || !name || !namelen)
		return false;
	if (!cached_dir_is_valid(cfid))
		return false;

	mutex_lock(&cfid->dirents.de_mutex);
	if (!cfid->dirents.is_valid || cfid->dirents.is_failed)
		goto out_unlock;

	invalidated = invalidate_cached_dirent_locked(&cfid->dirents,
							 name, namelen);

out_unlock:
	mutex_unlock(&cfid->dirents.de_mutex);
	return invalidated;
}

/*
 * Signal completion of dcache population for a specific dirent.
 * Called after cifs_prime_dcache returns, on both sync and async paths.
 * Clears the pending_dcache flag and unblocks any waiting lookups.
 */
void cifs_complete_pending_dcache(struct cached_fid *cfid,
		const char *name, unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	bool uses_folioq;
	int ret = -ENOENT;

	if (!cfid)
		return;

	mutex_lock(&cfid->dirents.de_mutex);
	uses_folioq = cached_dirents_use_folioq_locked(&cfid->dirents);
	entry = lookup_cached_dirent_entry_locked(&cfid->dirents, name, namelen);
	if (entry) {
		if (uses_folioq && entry->pending_dcache) {
			entry->pending_dcache = false;
			complete_all(&entry->dcache_complete);
		}
		ret = 0;
	}
	mutex_unlock(&cfid->dirents.de_mutex);
	cifs_dbg(FYI, "Dcache population of %.*s. status: %d\n",
					namelen, name, ret);
}

/*
 * Signal completion of dcache population for a specific dirent.
 * Wait for async dcache population to complete for a specific dirent.
 * Returns: 0 on completion or entry not pending, -ETIMEDOUT on timeout,
 *          -ENOENT if entry not found in the cache.
 */
int cifs_wait_for_pending_dcache(struct cached_fid *cfid,
		const char *name, unsigned int namelen)
{
	struct cached_dir_lookup_entry *entry;
	bool uses_folioq;
	struct completion *comp = NULL;
	int ret = -ENOENT;

	if (!cfid)
		return -ENOENT;

	mutex_lock(&cfid->dirents.de_mutex);
	uses_folioq = cached_dirents_use_folioq_locked(&cfid->dirents);
	entry = lookup_cached_dirent_entry_locked(&cfid->dirents, name, namelen);
	if (entry) {
		ret = 0;
		if (uses_folioq && entry->pending_dcache)
			comp = &entry->dcache_complete;
	}
	mutex_unlock(&cfid->dirents.de_mutex);

	if (comp) {
		if (wait_for_completion_timeout(comp, CIFS_DCACHE_WAIT_TIMEOUT) == 0) {
			cifs_dbg(FYI, "Timeout waiting for dcache population of %.*s\n",
					namelen, name);
			ret = -ETIMEDOUT;
		} else {
			cifs_dbg(FYI, "Dcache population completed for %.*s\n",
					namelen, name);
			ret = 0;
		}
	}

	return ret;
}

static struct cached_fid *find_or_create_cached_dir(struct cached_fids *cfids,
						    const char *path,
						    bool lookup_only,
						    __u32 max_cached_dirs)
{
	struct cached_fid *cfid;

	list_for_each_entry(cfid, &cfids->entries, entry) {
		if (!strcmp(cfid->path, path)) {
			/*
			 * If it doesn't have a lease it is either not yet
			 * fully cached or it may be in the process of
			 * being deleted due to a lease break.
			 */
			spin_lock(&cfid->cfid_lock);
			if (!is_valid_cached_dir(cfid)) {
				spin_unlock(&cfid->cfid_lock);
				return NULL;
			}
			kref_get(&cfid->refcount);
			spin_unlock(&cfid->cfid_lock);
			return cfid;
		}
	}
	if (lookup_only) {
		return NULL;
	}
	if (max_cached_dirs && cfids->num_entries >= max_cached_dirs) {
		return NULL;
	}
	cfid = init_cached_dir(path);
	if (cfid == NULL) {
		return NULL;
	}
	cfid->cfids = cfids;
	cfids->num_entries++;
	list_add(&cfid->entry, &cfids->entries);
	cfid->on_list = true;
	kref_get(&cfid->refcount);
	/*
	 * Set @cfid->has_lease to true during construction so that the lease
	 * reference can be put in cached_dir_lease_break() due to a potential
	 * lease break right after the request is sent or while @cfid is still
	 * being cached, or if a reconnection is triggered during construction.
	 * Concurrent processes won't be to use it yet due to @cfid->time being
	 * zero.
	 */
	spin_lock(&cfid->cfid_lock);
	cfid->has_lease = true;
	spin_unlock(&cfid->cfid_lock);

	return cfid;
}

static struct dentry *
path_to_dentry(struct cifs_sb_info *cifs_sb, const char *path)
{
	struct dentry *dentry;
	const char *s, *p;
	char sep;

	sep = CIFS_DIR_SEP(cifs_sb);
	dentry = dget(cifs_sb->root);
	s = path;

	do {
		struct inode *dir = d_inode(dentry);
		struct dentry *child;

		if (!S_ISDIR(dir->i_mode)) {
			dput(dentry);
			dentry = ERR_PTR(-ENOTDIR);
			break;
		}

		/* skip separators */
		while (*s == sep)
			s++;
		if (!*s)
			break;
		p = s++;
		/* next separator */
		while (*s && *s != sep)
			s++;

		child = lookup_noperm_positive_unlocked(&QSTR_LEN(p, s - p),
							dentry);
		dput(dentry);
		dentry = child;
	} while (!IS_ERR(dentry));
	return dentry;
}

static const char *path_no_prefix(struct cifs_sb_info *cifs_sb,
				  const char *path)
{
	size_t len = 0;

	if (!*path)
		return path;

	if ((cifs_sb_flags(cifs_sb) & CIFS_MOUNT_USE_PREFIX_PATH) &&
	    cifs_sb->prepath) {
		len = strlen(cifs_sb->prepath) + 1;
		if (unlikely(len > strlen(path)))
			return ERR_PTR(-EINVAL);
	}
	return path + len;
}

/*
 * Open the and cache a directory handle.
 * If error then *cfid is not initialized.
 */
int open_cached_dir(unsigned int xid, struct cifs_tcon *tcon,
		    const char *path,
		    struct cifs_sb_info *cifs_sb,
		    bool lookup_only, struct cached_fid **ret_cfid)
{
	struct cifs_ses *ses;
	struct TCP_Server_Info *server;
	struct cifs_open_parms oparms;
	struct smb2_create_rsp *o_rsp = NULL;
	struct smb2_query_info_rsp *qi_rsp = NULL;
	int resp_buftype[2];
	struct smb_rqst rqst[2];
	struct kvec rsp_iov[2];
	struct kvec open_iov[SMB2_CREATE_IOV_SIZE];
	struct kvec qi_iov[1];
	int rc, flags = 0;
	__le16 *utf16_path = NULL;
	u8 oplock = SMB2_OPLOCK_LEVEL_II;
	struct cifs_fid *pfid;
	struct dentry *dentry = NULL;
	struct cached_fid *cfid;
	struct cached_fids *cfids;
	const char *npath;
	int retries = 0, cur_sleep = 0;
	__le32 lease_flags = 0;

	if (cifs_sb->root == NULL)
		return -ENOENT;

	if (tcon == NULL)
		return -EOPNOTSUPP;

	ses = tcon->ses;
	cfids = tcon->cfids;

	if (cfids == NULL)
		return -EOPNOTSUPP;

replay_again:
	/* reinitialize for possible replay */
	flags = 0;
	oplock = SMB2_OPLOCK_LEVEL_II;
	server = cifs_pick_channel(ses);

	if (!server->ops->new_lease_key)
		return smb_EIO(smb_eio_trace_no_lease_key);

	utf16_path = cifs_convert_path_to_utf16(path, cifs_sb);
	if (!utf16_path)
		return -ENOMEM;

	spin_lock(&cfids->cfid_list_lock);
	cfid = find_or_create_cached_dir(cfids, path, lookup_only, tcon->max_cached_dirs);
	if (cfid == NULL) {
		spin_unlock(&cfids->cfid_list_lock);
		kfree(utf16_path);
		return -ENOENT;
	}
	spin_unlock(&cfids->cfid_list_lock);

	/*
	 * Return cached fid if it is valid (has a lease and has a time).
	 * Otherwise, it is either a new entry or laundromat worker removed it
	 * from @cfids->entries.  Caller will put last reference if the latter.
	 */

	spin_lock(&cfid->cfid_lock);
	if (is_valid_cached_dir(cfid)) {
		cfid->last_access_time = jiffies;
		spin_unlock(&cfid->cfid_lock);
		*ret_cfid = cfid;
		kfree(utf16_path);
		return 0;
	}
	spin_unlock(&cfid->cfid_lock);

	pfid = &cfid->fid;

	/*
	 * Skip any prefix paths in @path as lookup_noperm_positive_unlocked() ends up
	 * calling ->lookup() which already adds those through
	 * build_path_from_dentry().  Also, do it earlier as we might reconnect
	 * below when trying to send compounded request and then potentially
	 * having a different prefix path (e.g. after DFS failover).
	 */
	npath = path_no_prefix(cifs_sb, path);
	if (IS_ERR(npath)) {
		rc = PTR_ERR(npath);
		goto out;
	}

	if (!npath[0]) {
		dentry = dget(cifs_sb->root);
	} else {
		dentry = path_to_dentry(cifs_sb, npath);
		if (IS_ERR(dentry)) {
			rc = -ENOENT;
			goto out;
		}
		if (dentry->d_parent && server->dialect >= SMB30_PROT_ID) {
			struct cached_fid *parent_cfid;

			spin_lock(&cfids->cfid_list_lock);
			list_for_each_entry(parent_cfid, &cfids->entries, entry) {
				spin_lock(&parent_cfid->cfid_lock);
				if (parent_cfid->dentry == dentry->d_parent) {
					cifs_dbg(FYI, "found a parent cached file handle\n");
					if (is_valid_cached_dir(parent_cfid)) {
						lease_flags
							|= SMB2_LEASE_FLAG_PARENT_LEASE_KEY_SET_LE;
						memcpy(pfid->parent_lease_key,
						       parent_cfid->fid.lease_key,
						       SMB2_LEASE_KEY_SIZE);
					}
					spin_unlock(&parent_cfid->cfid_lock);
					break;
				}
				spin_unlock(&parent_cfid->cfid_lock);
			}
			spin_unlock(&cfids->cfid_list_lock);
		}
	}
	cfid->dentry = dentry;
	cfid->tcon = tcon;

	/*
	 * We do not hold the lock for the open because in case
	 * SMB2_open needs to reconnect.
	 * This is safe because no other thread will be able to get a ref
	 * to the cfid until we have finished opening the file and (possibly)
	 * acquired a lease.
	 */
	if (smb3_encryption_required(tcon))
		flags |= CIFS_TRANSFORM_REQ;

	server->ops->new_lease_key(pfid);

	memset(rqst, 0, sizeof(rqst));
	resp_buftype[0] = resp_buftype[1] = CIFS_NO_BUFFER;
	memset(rsp_iov, 0, sizeof(rsp_iov));

	/* Open */
	memset(&open_iov, 0, sizeof(open_iov));
	rqst[0].rq_iov = open_iov;
	rqst[0].rq_nvec = SMB2_CREATE_IOV_SIZE;

	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.path = path,
		.create_options = cifs_create_options(cifs_sb, CREATE_NOT_FILE),
		.desired_access =  FILE_READ_DATA | FILE_READ_ATTRIBUTES |
				   FILE_READ_EA,
		.disposition = FILE_OPEN,
		.fid = pfid,
		.lease_flags = lease_flags,
		.replay = !!(retries),
	};

	rc = SMB2_open_init(tcon, server,
			    &rqst[0], &oplock, &oparms, utf16_path);
	if (rc)
		goto oshr_free;

	if (oplock != SMB2_OPLOCK_LEVEL_II) {
		rc = -EINVAL;
		cifs_dbg(FYI, "%s: Oplock level %d not suitable for cached directory\n",
			 __func__, oplock);
		goto oshr_free;
	}

	smb2_set_next_command(tcon, &rqst[0]);

	memset(&qi_iov, 0, sizeof(qi_iov));
	rqst[1].rq_iov = qi_iov;
	rqst[1].rq_nvec = 1;

	rc = SMB2_query_info_init(tcon, server,
				  &rqst[1], COMPOUND_FID,
				  COMPOUND_FID, FILE_ALL_INFORMATION,
				  SMB2_O_INFO_FILE, 0,
				  sizeof(struct smb2_file_all_info) +
				  PATH_MAX * 2, 0, NULL);
	if (rc)
		goto oshr_free;

	smb2_set_related(&rqst[1]);

	if (retries) {
		/* Back-off before retry */
		if (cur_sleep)
			msleep(cur_sleep);

		smb2_set_replay(server, &rqst[0]);
		smb2_set_replay(server, &rqst[1]);
	}

	mutex_lock(&cfid->cfid_open_mutex);

	rc = compound_send_recv(xid, ses, server,
				flags, 2, rqst,
				resp_buftype, rsp_iov);
	if (rc) {
		mutex_unlock(&cfid->cfid_open_mutex);
		if (rc == -EREMCHG) {
			tcon->need_reconnect = true;
			pr_warn_once("server share %s deleted\n",
				     tcon->tree_name);
		}
		goto oshr_free;
	}
	spin_lock(&cfid->cfid_lock);
	cfid->is_open = true;

	o_rsp = (struct smb2_create_rsp *)rsp_iov[0].iov_base;
	oparms.fid->persistent_fid = o_rsp->PersistentFileId;
	oparms.fid->volatile_fid = o_rsp->VolatileFileId;
#ifdef CONFIG_CIFS_DEBUG2
	oparms.fid->mid = le64_to_cpu(o_rsp->hdr.MessageId);
#endif /* CIFS_DEBUG2 */


	if (o_rsp->OplockLevel != SMB2_OPLOCK_LEVEL_LEASE) {
		rc = -EINVAL;
		spin_unlock(&cfid->cfid_lock);
		mutex_unlock(&cfid->cfid_open_mutex);
		goto oshr_free;
	}

	rc = smb2_parse_contexts(server, rsp_iov,
				 &oparms.fid->epoch,
				 oparms.fid->lease_key,
				 &oplock, NULL, NULL);
	if (rc) {
		spin_unlock(&cfid->cfid_lock);
		mutex_unlock(&cfid->cfid_open_mutex);
		goto oshr_free;
	}

	rc = -EINVAL;
	if (!(oplock & SMB2_LEASE_READ_CACHING_HE)) {
		spin_unlock(&cfid->cfid_lock);
		mutex_unlock(&cfid->cfid_open_mutex);
		goto oshr_free;
	}
	qi_rsp = (struct smb2_query_info_rsp *)rsp_iov[1].iov_base;
	if (le32_to_cpu(qi_rsp->OutputBufferLength) < sizeof(struct smb2_file_all_info)) {
		spin_unlock(&cfid->cfid_lock);
		mutex_unlock(&cfid->cfid_open_mutex);
		goto oshr_free;
	}
	if (!smb2_validate_and_copy_iov(
				le16_to_cpu(qi_rsp->OutputBufferOffset),
				sizeof(struct smb2_file_all_info),
				&rsp_iov[1], sizeof(struct smb2_file_all_info),
				(char *)&cfid->file_all_info))
		cfid->file_all_info_is_valid = true;

	cfid->time = jiffies;
	cfid->last_access_time = jiffies;
	spin_unlock(&cfid->cfid_lock);
	mutex_unlock(&cfid->cfid_open_mutex);
	/* At this point the directory handle is fully cached */
	rc = 0;

oshr_free:
	SMB2_open_free(&rqst[0]);
	SMB2_query_info_free(&rqst[1]);
	free_rsp_buf(resp_buftype[0], rsp_iov[0].iov_base);
	free_rsp_buf(resp_buftype[1], rsp_iov[1].iov_base);
out:
	if (rc) {
		bool drop_lease_ref = false;

		spin_lock(&cfids->cfid_list_lock);
		if (cfid->on_list) {
			list_del(&cfid->entry);
			cfid->on_list = false;
			cfids->num_entries--;
		}
		spin_lock(&cfid->cfid_lock);
		if (cfid->has_lease) {
			cfid->has_lease = false;
			drop_lease_ref = true;
		}
		spin_unlock(&cfid->cfid_lock);
		spin_unlock(&cfids->cfid_list_lock);

		if (drop_lease_ref)
			close_cached_dir(cfid);
		close_cached_dir(cfid);
	} else {
		*ret_cfid = cfid;
		atomic_inc(&tcon->num_remote_opens);
	}
	kfree(utf16_path);

	if (is_replayable_error(rc) &&
	    smb2_should_replay(tcon, &retries, &cur_sleep))
		goto replay_again;

	return rc;
}

int open_cached_dir_by_dentry(struct cifs_tcon *tcon,
			      struct dentry *dentry,
			      struct cached_fid **ret_cfid)
{
	struct cached_fid *cfid;
	struct cached_fid *trace_cfid = NULL;
	struct cached_fids *cfids = tcon->cfids;
	int rc = -ENOENT;

	if (cfids == NULL)
		return -EOPNOTSUPP;

	if (!dentry)
		return -ENOENT;

	spin_lock(&cfids->cfid_list_lock);
	list_for_each_entry(cfid, &cfids->entries, entry) {
		if (cfid->dentry == dentry) {
			spin_lock(&cfid->cfid_lock);
			if (!is_valid_cached_dir(cfid)) {
				spin_unlock(&cfid->cfid_lock);
				break;
			}
			cifs_dbg(FYI, "found a cached file handle by dentry\n");
			kref_get(&cfid->refcount);
			*ret_cfid = cfid;
			cfid->last_access_time = jiffies;
			rc = 0;
			trace_cfid = cfid;
			spin_unlock(&cfid->cfid_lock);
			spin_unlock(&cfids->cfid_list_lock);
			return rc;
		}
	}
	spin_unlock(&cfids->cfid_list_lock);
	return rc;
}

static void
smb2_close_cached_fid(struct kref *ref)
__releases(&cfid->cfids->cfid_list_lock)
{
	struct cached_fid *cfid = container_of(ref, struct cached_fid,
					       refcount);
	u64 persistent_fid = 0, volatile_fid = 0;
	bool is_open;
	int rc;

	lockdep_assert_held(&cfid->cfids->cfid_list_lock);

	if (cfid->on_list) {
		list_del(&cfid->entry);
		cfid->on_list = false;
		cfid->cfids->num_entries--;
	}
	spin_unlock(&cfid->cfids->cfid_list_lock);

	dput(cfid->dentry);
	cfid->dentry = NULL;

	spin_lock(&cfid->cfid_lock);
	is_open = cfid->is_open;
	if (is_open) {
		persistent_fid = cfid->fid.persistent_fid;
		volatile_fid = cfid->fid.volatile_fid;
		cfid->is_open = false;
	}
	spin_unlock(&cfid->cfid_lock);

	if (is_open) {
		rc = SMB2_close(0, cfid->tcon, persistent_fid, volatile_fid);
		if (rc) /* should we retry on -EBUSY or -EAGAIN? */
			cifs_dbg(VFS, "close cached dir rc %d\n", rc);
	}

	free_cached_dir(cfid);
}

void drop_cached_dir_by_name(const unsigned int xid, struct cifs_tcon *tcon,
			     const char *name, struct cifs_sb_info *cifs_sb)
{
	struct cached_fid *cfid = NULL;
	int rc;
	bool drop_lease_ref = false;

	rc = open_cached_dir(xid, tcon, name, cifs_sb, true, &cfid);
	if (rc) {
		cifs_dbg(FYI, "no cached dir found for rmdir(%s)\n", name);
		return;
	}
	spin_lock(&cfid->cfids->cfid_list_lock);
	spin_lock(&cfid->cfid_lock);
	if (cfid->has_lease) {
		cfid->has_lease = false;
		drop_lease_ref = true;
	}
	spin_unlock(&cfid->cfid_lock);
	spin_unlock(&cfid->cfids->cfid_list_lock);

	if (drop_lease_ref)
		close_cached_dir(cfid);
	close_cached_dir(cfid);
}

/**
 * close_cached_dir - drop a reference of a cached dir
 *
 * The release function will be called with cfid_list_lock held to remove the
 * cached dirs from the list before any other thread can take another @cfid
 * ref. Must not be called with cfid_list_lock held.
 *
 * @cfid: cached dir
 */
void close_cached_dir(struct cached_fid *cfid)
{
	lockdep_assert_not_held(&cfid->cfids->cfid_list_lock);
	kref_put_lock(&cfid->refcount, smb2_close_cached_fid, &cfid->cfids->cfid_list_lock);
}

/*
 * Called from cifs_kill_sb when we unmount a share
 */
void close_all_cached_dirs(struct cifs_sb_info *cifs_sb)
{
	struct rb_root *root = &cifs_sb->tlink_tree;
	struct rb_node *node;
	struct cached_fid *cfid;
	struct cifs_tcon *tcon;
	struct tcon_link *tlink;
	struct cached_fids *cfids;
	struct cached_dir_dentry *tmp_list, *q;
	LIST_HEAD(entry);

	spin_lock(&cifs_sb->tlink_tree_lock);
	for (node = rb_first(root); node; node = rb_next(node)) {
		tlink = rb_entry(node, struct tcon_link, tl_rbnode);
		tcon = tlink_tcon(tlink);
		if (IS_ERR(tcon))
			continue;
		cfids = tcon->cfids;
		if (cfids == NULL)
			continue;
		spin_lock(&cfids->cfid_list_lock);
		list_for_each_entry(cfid, &cfids->entries, entry) {
			tmp_list = kmalloc_obj(*tmp_list, GFP_ATOMIC);
			if (tmp_list == NULL) {
				/*
				 * If the malloc() fails, we won't drop all
				 * dentries, and unmounting is likely to trigger
				 * a 'Dentry still in use' error.
				 */
				cifs_tcon_dbg(VFS, "Out of memory while dropping dentries\n");
				spin_unlock(&cfids->cfid_list_lock);
				spin_unlock(&cifs_sb->tlink_tree_lock);
				goto done;
			}

			spin_lock(&cfid->cfid_lock);
			tmp_list->dentry = cfid->dentry;
			cfid->dentry = NULL;
			spin_unlock(&cfid->cfid_lock);

			list_add_tail(&tmp_list->entry, &entry);
		}
		spin_unlock(&cfids->cfid_list_lock);
	}
	spin_unlock(&cifs_sb->tlink_tree_lock);

done:
	list_for_each_entry_safe(tmp_list, q, &entry, entry) {
		list_del(&tmp_list->entry);
		dput(tmp_list->dentry);
		kfree(tmp_list);
	}

	/* Flush any pending work that will drop dentries */
	flush_workqueue(cfid_put_wq);
}

/*
 * Queue all cached dirs for invalidation on laundromat without waiting.
 * Safe for callers that hold cifs_tcp_ses_lock.
 */
void invalidate_all_cached_dirs_nowait(struct cifs_tcon *tcon)
{
	struct cached_fids *cfids = tcon->cfids;
	struct cached_fid *cfid, *q;

	if (cfids == NULL)
		return;

	/*
	 * Mark all the cfids as closed, and move them to the cfids->dying list.
	 * They'll be cleaned up by laundromat.  Take a reference to each cfid
	 * during this process.
	 */
	spin_lock(&cfids->cfid_list_lock);
	list_for_each_entry_safe(cfid, q, &cfids->entries, entry) {
		list_move(&cfid->entry, &cfids->dying);
		cfids->num_entries--;
		spin_lock(&cfid->cfid_lock);
		cfid->is_open = false;
		if (cfid->has_lease) {
			/*
			 * The lease was never cancelled from the server,
			 * so steal that reference.
			 */
			cfid->has_lease = false;
			spin_unlock(&cfid->cfid_lock);
		} else {
			spin_unlock(&cfid->cfid_lock);
			kref_get(&cfid->refcount);
		}
		cfid->on_list = false;
	}
	spin_unlock(&cfids->cfid_list_lock);

	/* Run laundromat now as there might have been previously queued work. */
	mod_delayed_work(cfid_put_wq, &cfids->laundromat_work, 0);
}

/*
 * Invalidate all cached dirs when a TCON has been reset
 * due to a session loss.
 */
void invalidate_all_cached_dirs(struct cifs_tcon *tcon)
{
	struct cached_fids *cfids = tcon->cfids;

	if (!cfids)
		return;

	invalidate_all_cached_dirs_nowait(tcon);
	flush_delayed_work(&cfids->laundromat_work);
}

static void
cached_dir_offload_close(struct work_struct *work)
{
	struct cached_fid *cfid = container_of(work,
				struct cached_fid, close_work);
	struct cifs_tcon *tcon = cfid->tcon;

	WARN_ON(cfid->on_list);

	close_cached_dir(cfid);
	cifs_put_tcon(tcon, netfs_trace_tcon_ref_put_cached_close);
}

/*
 * Release the cached directory's dentry, and then queue work to drop cached
 * directory itself (closing on server if needed).
 *
 * Must be called with a reference to the cached_fid and a reference to the
 * tcon.
 */
static void cached_dir_put_work(struct work_struct *work)
{
	struct cached_fid *cfid = container_of(work, struct cached_fid,
					       put_work);
	dput(cfid->dentry);
	cfid->dentry = NULL;

	queue_work(serverclose_wq, &cfid->close_work);
}

bool cached_dir_lease_break(struct cifs_tcon *tcon, __u8 lease_key[16])
{
	struct cached_fids *cfids = tcon->cfids;
	struct cached_fid *cfid;

	if (cfids == NULL)
		return false;

	spin_lock(&cfids->cfid_list_lock);
	list_for_each_entry(cfid, &cfids->entries, entry) {
		spin_lock(&cfid->cfid_lock);
		if (cfid->has_lease &&
		    !memcmp(lease_key,
			    cfid->fid.lease_key,
			    SMB2_LEASE_KEY_SIZE)) {
			cfid->has_lease = false;
			cfid->time = 0;
			spin_unlock(&cfid->cfid_lock);
			/*
			 * We found a lease remove it from the list
			 * so no threads can access it.
			 */
			list_del(&cfid->entry);
			cfid->on_list = false;
			cfids->num_entries--;

			++tcon->tc_count;
			trace_smb3_tcon_ref(tcon->debug_id, tcon->tc_count,
					    netfs_trace_tcon_ref_get_cached_lease_break);
			queue_work(cfid_put_wq, &cfid->put_work);
			spin_unlock(&cfids->cfid_list_lock);
			return true;
		}
		spin_unlock(&cfid->cfid_lock);
	}
	spin_unlock(&cfids->cfid_list_lock);
	return false;
}

static struct cached_fid *init_cached_dir(const char *path)
{
	struct cached_fid *cfid;

	cfid = kzalloc_obj(*cfid, GFP_ATOMIC);
	if (!cfid)
		return NULL;
	cfid->path = kstrdup(path, GFP_ATOMIC);
	if (!cfid->path) {
		kfree(cfid);
		return NULL;
	}

	INIT_WORK(&cfid->close_work, cached_dir_offload_close);
	INIT_WORK(&cfid->put_work, cached_dir_put_work);
	INIT_LIST_HEAD(&cfid->entry);
	INIT_LIST_HEAD(&cfid->dirents.entry_list);
	mutex_init(&cfid->dirents.de_mutex);
	mutex_init(&cfid->cfid_open_mutex);
	spin_lock_init(&cfid->cfid_lock);
	kref_init(&cfid->refcount);
	return cfid;
}

static void free_cached_dir(struct cached_fid *cfid)
{
	unsigned long entries_count = 0;
	u64 bytes_used = 0;

	WARN_ON(work_pending(&cfid->close_work));
	WARN_ON(work_pending(&cfid->put_work));


	dput(cfid->dentry);
	cfid->dentry = NULL;

	mutex_lock(&cfid->dirents.de_mutex);
	entries_count = cfid->dirents.entries_count;
	bytes_used = cfid->dirents.bytes_used;
	release_cached_dirents_locked(&cfid->dirents);
	mutex_unlock(&cfid->dirents.de_mutex);

	/* adjust tcon-level counters and reset per-dir accounting */
	if (cfid->cfids) {
		if (entries_count)
			atomic_long_sub((long)entries_count,
					&cfid->cfids->total_dirents_entries);
		if (bytes_used) {
			atomic64_sub((long long)bytes_used,
					&cfid->cfids->total_dirents_bytes);
			atomic64_sub((long long)bytes_used,
					&cifs_dircache_bytes_used);
		}
	}
	kfree(cfid->path);
	cfid->path = NULL;
	kfree(cfid);
}

static void cfids_laundromat_worker(struct work_struct *work)
{
	struct cached_fids *cfids;
	struct cached_fid *cfid, *q;
	LIST_HEAD(entry);

	cfids = container_of(work, struct cached_fids, laundromat_work.work);

	spin_lock(&cfids->cfid_list_lock);
	/* move cfids->dying to the local list */
	list_cut_before(&entry, &cfids->dying, &cfids->dying);

	list_for_each_entry_safe(cfid, q, &cfids->entries, entry) {
		spin_lock(&cfid->cfid_lock);
		if (dir_cache_timeout && cfid->last_access_time &&
		    time_after(jiffies, cfid->last_access_time + HZ * dir_cache_timeout)) {
			cfid->on_list = false;
			list_move(&cfid->entry, &entry);
			cfids->num_entries--;
			if (cfid->has_lease) {
				/*
				 * Our lease has not yet been cancelled from the
				 * server. Steal that reference.
				 */
				cfid->has_lease = false;
				spin_unlock(&cfid->cfid_lock);
			} else {
				spin_unlock(&cfid->cfid_lock);
				kref_get(&cfid->refcount);
			}
		} else {
			spin_unlock(&cfid->cfid_lock);
		}
	}
	spin_unlock(&cfids->cfid_list_lock);

	list_for_each_entry_safe(cfid, q, &entry, entry) {
		list_del(&cfid->entry);

		dput(cfid->dentry);
		cfid->dentry = NULL;

		if (cfid->is_open) {
			spin_lock(&cfid->tcon->tc_lock);
			++cfid->tcon->tc_count;
			trace_smb3_tcon_ref(cfid->tcon->debug_id, cfid->tcon->tc_count,
					    netfs_trace_tcon_ref_get_cached_laundromat);
			spin_unlock(&cfid->tcon->tc_lock);
			queue_work(serverclose_wq, &cfid->close_work);
		} else
			/*
			 * Drop the ref-count from above, either the lease-ref (if there
			 * was one) or the extra one acquired.
			 */
			close_cached_dir(cfid);
	}
	if (dir_cache_timeout)
		queue_delayed_work(cfid_put_wq, &cfids->laundromat_work,
				   dir_cache_timeout * HZ);
}

struct cached_fids *init_cached_dirs(void)
{
	struct cached_fids *cfids;

	cfids = kzalloc_obj(*cfids);
	if (!cfids)
		return NULL;
	spin_lock_init(&cfids->cfid_list_lock);
	INIT_LIST_HEAD(&cfids->entries);
	INIT_LIST_HEAD(&cfids->dying);

	INIT_DELAYED_WORK(&cfids->laundromat_work, cfids_laundromat_worker);
	if (dir_cache_timeout)
		queue_delayed_work(cfid_put_wq, &cfids->laundromat_work,
				   dir_cache_timeout * HZ);

	atomic_long_set(&cfids->total_dirents_entries, 0);
	atomic64_set(&cfids->total_dirents_bytes, 0);

	return cfids;
}

/*
 * Called from tconInfoFree when we are tearing down the tcon.
 * There are no active users or open files/directories at this point.
 */
void free_cached_dirs(struct cached_fids *cfids)
{
	struct cached_fid *cfid, *q;
	LIST_HEAD(entry);

	if (cfids == NULL)
		return;

	cancel_delayed_work_sync(&cfids->laundromat_work);

	spin_lock(&cfids->cfid_list_lock);
	list_for_each_entry_safe(cfid, q, &cfids->entries, entry) {
		cfid->on_list = false;
		spin_lock(&cfid->cfid_lock);
		cfid->is_open = false;
		spin_unlock(&cfid->cfid_lock);
		list_move(&cfid->entry, &entry);
	}
	list_for_each_entry_safe(cfid, q, &cfids->dying, entry) {
		cfid->on_list = false;
		spin_lock(&cfid->cfid_lock);
		cfid->is_open = false;
		spin_unlock(&cfid->cfid_lock);
		list_move(&cfid->entry, &entry);
	}
	spin_unlock(&cfids->cfid_list_lock);

	list_for_each_entry_safe(cfid, q, &entry, entry) {
		list_del(&cfid->entry);
		free_cached_dir(cfid);
	}

	kfree(cfids);
}

void cifs_set_srch_inf_cfid(struct cifs_search_info *srch_inf,
			   struct cached_fid *cfid)
{
	if (srch_inf->cfid == cfid)
		return;

	if (cfid)
		kref_get(&cfid->refcount);

	if (srch_inf->cfid)
		close_cached_dir(srch_inf->cfid);

	srch_inf->cfid = cfid;
}

void cifs_put_srch_inf_cfid(struct cifs_search_info *srch_inf)
{
	if (!srch_inf->cfid)
		return;

	close_cached_dir(srch_inf->cfid);
	srch_inf->cfid = NULL;
}
