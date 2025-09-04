/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Functions to handle the cached directory entries
 *
 *  Copyright (c) 2022, Ronnie Sahlberg <lsahlber@redhat.com>
 */

#ifndef _CACHED_DIR_H
#define _CACHED_DIR_H

struct cached_dirent {
	struct list_head entry;
	char *name;
	int namelen;
	loff_t pos;
	struct cifs_fattr fattr;
};

struct cached_dirents {
	bool is_valid:1;
	bool is_failed:1;
	struct file *file; /*
			    * Used to associate the cache with a single
			    * open file instance.
			    */
	struct mutex de_mutex;
	loff_t pos;		 /* Expected ctx->pos */
	struct list_head entries;
};

struct cached_fid {
	struct list_head entry;
	struct cached_fids *cfids;
	const char *path;
	bool has_lease:1;
	bool is_open:1;
	bool on_list:1;
	bool file_all_info_is_valid:1;
	unsigned long time; /* jiffies of when lease was taken */
	unsigned long last_access_time; /* jiffies of when last accessed */
	struct kref refcount;
	struct cifs_fid fid;
	spinlock_t fid_lock;
	struct cifs_tcon *tcon;
	struct dentry *dentry;
	struct work_struct put_work;
	struct work_struct close_work;
	struct smb2_file_all_info file_all_info;
	struct cached_dirents dirents;
};

/* default MAX_CACHED_FIDS is 16 */
struct cached_fids {
	/* Must be held when:
	 * - accessing the cfids->entries list
	 * - accessing the cfids->dying list
	 */
	spinlock_t cfid_list_lock;
	int num_entries;
	struct list_head entries;
	struct list_head dying;
	struct work_struct invalidation_work;
	struct delayed_work laundromat_work;
};

#endif			/* _CACHED_DIR_H */
