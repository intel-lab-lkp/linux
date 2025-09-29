// SPDX-License-Identifier: GPL-2.0
/*
 *  Functions to handle the cached directory entries
 *
 *  Copyright (c) 2022, Ronnie Sahlberg <lsahlber@redhat.com>
 */

#include <linux/namei.h>
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "smb2proto.h"
#include "cached_dir.h"

/* since jiffies can never be 1 here, use it as an invalid value to add meaning for our purposes */
#define CFID_INVALID_TIME 1

static struct cached_fid *init_cached_dir(const char *path);
static void cfid_release_ref(struct kref *ref);

static inline void invalidate_cfid(struct cached_fid *cfid)
{
	/* callers must hold the list lock and do any list operations (del/move) themselves */
	lockdep_assert_held(&cfid->cfids->entries_seqlock.lock);

	if (cfid_is_valid(cfid))
		cfid->cfids->num_entries--;

	/* do not change other fields here! */
	cfid->ctime = CFID_INVALID_TIME;
	cfid->atime = CFID_INVALID_TIME;
}

static inline void __drop_cfid(struct cached_fid *cfid)
{
	struct dentry *dentry = NULL;
	u64 pfid = 0, vfid = 0;

	write_seqlock(&cfid->cfids->entries_seqlock);
	write_seqlock(&cfid->seqlock);
	invalidate_cfid(cfid);
	swap(cfid->dentry, dentry);
	swap(cfid->fid.persistent_fid, pfid);
	swap(cfid->fid.volatile_fid, vfid);
	write_sequnlock(&cfid->seqlock);
	write_sequnlock(&cfid->cfids->entries_seqlock);

	dput(dentry);

	if (pfid) {
		/* cfid->tcon is never set to NULL, so no need to check/swap it */
		int rc = SMB2_close(0, cfid->tcon, pfid, vfid);

		/* SMB2_close should handle -EBUSY or -EAGAIN */
		if (rc)
			cifs_dbg(VFS, "close cached dir rc %d\n", rc);
	}
}

static inline void drop_cfid(struct cached_fid *cfid)
{
	__drop_cfid(cfid);
	kref_put(&cfid->refcount, cfid_release_ref);
}

/*
 * Find a cached dir based on @key and @mode (raw lookup).
 * The only validation done here is if cfid is going down (->ctime == CFID_INVALID_TIME).
 *
 * If @wait_open is true, keep retrying until cfid transitions from 'opening' to valid/invalid.
 * Will also keep retrying on list seqcount invalidations.
 *
 * Callers must handle any other validation as needed.
 * Returned cfid, if found, has a ref taken, regardless of state.
 */
static struct cached_fid *find_cfid(struct cached_fids *cfids, const void *key, int mode,
				    bool wait_open)
{
	struct cached_fid *cfid;
	unsigned int lseq = 0;
	int ret;

	if (!cfids || !key)
		return NULL;

retry_find:
	ret = -ENOENT;

	rcu_read_lock();
	lseq = read_seqbegin(&cfids->entries_seqlock);
	list_for_each_entry_rcu(cfid, &cfids->entries, entry) {
		if (need_seqretry(&cfids->entries_seqlock, lseq)) {
			ret = -ECHILD;
			break;
		}

		ret = -ENOENT;
		/* don't even bother checking if it's going away */
		if (cfid->ctime == CFID_INVALID_TIME)
			continue;

		if (mode == CFID_LOOKUP_PATH)
			ret = strcmp(cfid->path, (char *)key);

		if (mode == CFID_LOOKUP_DENTRY)
			ret = (cfid->dentry != key);

		if (mode == CFID_LOOKUP_LEASEKEY)
			ret = memcmp(cfid->fid.lease_key, (u8 *)key, SMB2_LEASE_KEY_SIZE);

		if (ret) {
			ret = -ENOENT;
			continue;
		}

		if (wait_open && !cfid->ctime) {
			unsigned int cseq = read_seqbegin(&cfid->seqlock);

			if (!cfid->ctime)
				ret = -ECHILD;
			else if (!cfid_is_valid(cfid))
				ret = -EINVAL;

			if (read_seqretry(&cfid->seqlock, cseq) && !ret)
				ret = -ECHILD;

			if (ret)
				break;
		}

		kref_get(&cfid->refcount);
		break;
	}

	if (read_seqretry(&cfids->entries_seqlock, lseq)) {
		if (wait_open) {
			if (ret == -ENOENT) {
				ret = -ECHILD;

				/*
				 * Not found but caller requested wait for open.
				 * The list seqcount invalidation might have been our open, retry
				 * only once more (in case it wasn't).
				 */
				wait_open = false;
			}
		}

		if (!ret)
			ret = -EUCLEAN;
	}
	rcu_read_unlock();

	if (!ret)
		return cfid;

	if (ret == -EUCLEAN) {
		kref_put(&cfid->refcount, cfid_release_ref);
		ret = -ECHILD;
	}

	if (ret == -ECHILD)
		goto retry_find;

	/* -EINVAL or -ENOENT */
	return NULL;
}

/*
 * Skip any prefix paths in @path as lookup_noperm_positive_unlocked() ends up calling ->lookup()
 * which already adds those through build_path_from_dentry().
 *
 * Also, this should be called before sending a network request as we might reconnect and
 * potentially end up having a different prefix path (e.g. after DFS failover).
 *
 * Callers must dput() returned dentry if !IS_ERR().
 */
static struct dentry *path_to_dentry(struct cifs_sb_info *cifs_sb, const char *path)
{
	struct dentry *dentry;
	const char *s, *p;
	char sep;

	if (!*path)
		return dget(cifs_sb->root);

	if ((cifs_sb->mnt_cifs_flags & CIFS_MOUNT_USE_PREFIX_PATH) && cifs_sb->prepath) {
		size_t len = strlen(cifs_sb->prepath) + 1;

		if (unlikely(len > strlen(path)))
			return ERR_PTR(-EINVAL);

		path += len;
	}

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

		child = lookup_noperm_positive_unlocked(&QSTR_LEN(p, s - p), dentry);
		dput(dentry);
		dentry = child;
	} while (!IS_ERR(dentry));

	return dentry;
}

/*
 * Find a cached dir based on @key and @mode (caller exposed).
 * This function will retry lookup if cfid found is in opening state.
 *
 * Returns valid cfid (with updated last_access_time) or NULL.
 */
struct cached_fid *find_cached_dir(struct cached_fids *cfids, const void *key, int mode)
{
	struct cached_fid *cfid;

	if (!cfids || !key)
		return NULL;

	cfid = find_cfid(cfids, key, mode, true);
	if (cfid) {
		if (cfid_is_valid(cfid)) {
			cfid->atime = jiffies;
		} else {
			kref_put(&cfid->refcount, cfid_release_ref);
			cfid = NULL;
		}
	}

	return cfid;
}

/*
 * Open the and cache a directory handle.
 * If error then *cfid is not initialized.
 */
int open_cached_dir(unsigned int xid, struct cifs_tcon *tcon, const char *path,
		    struct cifs_sb_info *cifs_sb, struct cached_fid **ret_cfid)
{
	int rc, flags = 0, retries = 0, cur_sleep = 1;
	struct kvec open_iov[SMB2_CREATE_IOV_SIZE];
	struct smb2_query_info_rsp *qi_rsp = NULL;
	struct TCP_Server_Info *server;
	struct cifs_open_parms oparms;
	struct smb2_file_all_info info;
	struct smb2_create_rsp *o_rsp = NULL;
	struct cached_fid __rcu *cfid;
	struct cached_fids *cfids;
	struct smb_rqst rqst[2];
	struct kvec rsp_iov[2];
	struct cifs_fid *pfid;
	struct dentry *dentry;
	struct kvec qi_iov[1];
	struct cifs_ses *ses;
	int resp_buftype[2];
	__le16 *utf16_path = NULL;
	u8 oplock = SMB2_OPLOCK_LEVEL_II;

	if (!cifs_sb->root)
		return -ENOENT;

	if (!tcon)
		return -EOPNOTSUPP;

	ses = tcon->ses;
	cfids = tcon->cfids;
	if (!cfids)
		return -EOPNOTSUPP;

replay_again:
	/* reinitialize for possible replay */
	flags = 0;
	oplock = SMB2_OPLOCK_LEVEL_II;
	dentry = NULL;
	cfid = NULL;
	*ret_cfid = NULL;
	memset(&info, 0, sizeof(info));
	server = cifs_pick_channel(ses);

	if (!server->ops->new_lease_key)
		return -EIO;

	/* find_cached_dir() already validates cfid if found, so no need to check here again */
	cfid = find_cached_dir(cfids, path, CFID_LOOKUP_PATH);
	if (cfid) {
		*ret_cfid = cfid;
		return 0;
	}

	read_seqlock_excl(&cfids->entries_seqlock);
	if (cfids->num_entries >= tcon->max_cached_dirs) {
		read_sequnlock_excl(&cfids->entries_seqlock);
		rc = -ENOENT;
		goto out;
	}
	read_sequnlock_excl(&cfids->entries_seqlock);

	/* no ned to lock cfid or entries yet */
	cfid = init_cached_dir(path);
	if (!cfid) {
		rc = -ENOMEM;
		goto out;
	}

	cfid->cfids = cfids;
	cfid->tcon = tcon;
	pfid = &cfid->fid;

	dentry = path_to_dentry(cifs_sb, path);
	if (IS_ERR(dentry)) {
		rc = PTR_ERR(dentry);
		dentry = NULL;
		goto out;
	}

	write_seqlock(&cfids->entries_seqlock);
	cfids->num_entries++;
	list_add_rcu(&cfid->entry, &cfids->entries);
	write_sequnlock(&cfids->entries_seqlock);

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

	oparms = CIFS_OPARMS(cifs_sb, tcon, path,
			     FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA, FILE_OPEN,
			     cifs_create_options(cifs_sb, CREATE_NOT_FILE), 0);
	oparms.fid = pfid;
	oparms.replay = !!retries;

	utf16_path = cifs_convert_path_to_utf16(path, cifs_sb);
	if (!utf16_path) {
		rc = -ENOMEM;
		goto oshr_free;
	}

	rc = SMB2_open_init(tcon, server, &rqst[0], &oplock, &oparms, utf16_path);
	kfree(utf16_path);

	if (rc)
		goto oshr_free;

	smb2_set_next_command(tcon, &rqst[0]);
	memset(&qi_iov, 0, sizeof(qi_iov));
	rqst[1].rq_iov = qi_iov;
	rqst[1].rq_nvec = 1;

	rc = SMB2_query_info_init(tcon, server, &rqst[1], COMPOUND_FID, COMPOUND_FID,
				  FILE_ALL_INFORMATION, SMB2_O_INFO_FILE, 0,
				  sizeof(struct smb2_file_all_info) + PATH_MAX * 2, 0, NULL);
	if (rc)
		goto oshr_free;

	smb2_set_related(&rqst[1]);

	if (retries) {
		smb2_set_replay(server, &rqst[0]);
		smb2_set_replay(server, &rqst[1]);
	}

	rc = compound_send_recv(xid, ses, server, flags, 2, rqst, resp_buftype, rsp_iov);
	if (rc) {
		if (rc == -EREMCHG) {
			tcon->need_reconnect = true;
			pr_warn_once("server share %s deleted\n", tcon->tree_name);
		}
		goto oshr_free;
	}

	o_rsp = (struct smb2_create_rsp *)rsp_iov[0].iov_base;
	oparms.fid->persistent_fid = o_rsp->PersistentFileId;
	oparms.fid->volatile_fid = o_rsp->VolatileFileId;
#ifdef CONFIG_CIFS_DEBUG2
	oparms.fid->mid = le64_to_cpu(o_rsp->hdr.MessageId);
#endif /* CIFS_DEBUG2 */

	if (o_rsp->OplockLevel != SMB2_OPLOCK_LEVEL_LEASE) {
		rc = -EINVAL;
		goto oshr_free;
	}

	rc = smb2_parse_contexts(server, rsp_iov, &oparms.fid->epoch, oparms.fid->lease_key,
				 &oplock, NULL, NULL);
	if (rc)
		goto oshr_free;

	rc = -EINVAL;
	if (!(oplock & SMB2_LEASE_READ_CACHING_HE))
		goto oshr_free;

	qi_rsp = (struct smb2_query_info_rsp *)rsp_iov[1].iov_base;
	if (le32_to_cpu(qi_rsp->OutputBufferLength) < sizeof(struct smb2_file_all_info))
		goto oshr_free;

	if (!smb2_validate_and_copy_iov(le16_to_cpu(qi_rsp->OutputBufferOffset), sizeof(info),
					&rsp_iov[1], sizeof(info), (char *)&info)) {
		cfid->file_all_info = kmemdup(&info, sizeof(info), GFP_ATOMIC);
		if (!cfid->file_all_info) {
			rc = -ENOMEM;
			goto out;
		}
	}

	rc = 0;
oshr_free:
	SMB2_open_free(&rqst[0]);
	SMB2_query_info_free(&rqst[1]);
	free_rsp_buf(resp_buftype[0], rsp_iov[0].iov_base);
	free_rsp_buf(resp_buftype[1], rsp_iov[1].iov_base);
out:
	/* cfid only becomes fully valid below, so can't use cfid_is_valid() here */
	if (!rc && cfid->ctime == CFID_INVALID_TIME)
		rc = -ENOENT;

	if (rc) {
		dput(dentry);

		if (cfid)
			drop_cfid(cfid);
	} else {
		/* seqlocked-write will inform concurrent lookups of opening -> open transition */
		write_seqlock(&cfid->seqlock);
		cfid->dentry = dentry;
		cfid->ctime = jiffies;
		cfid->atime = jiffies;
		write_sequnlock(&cfid->seqlock);

		*ret_cfid = cfid;
		atomic_inc(&tcon->num_remote_opens);
	}

	if (is_replayable_error(rc) &&
	    smb2_should_replay(tcon, &retries, &cur_sleep))
		goto replay_again;

	return rc;
}

static void cfid_rcu_free(struct rcu_head *rcu)
{
	struct cached_fid *cfid = container_of(rcu, struct cached_fid, rcu);
	struct cached_dirent *de, *q;

	/* Delete all cached dirent names */
	list_for_each_entry_safe(de, q, &cfid->dirents.entries, entry) {
		list_del(&de->entry);
		kfree(de->name);
		kfree(de);
	}

	kfree(cfid->file_all_info);
	cfid->file_all_info = NULL;
	kfree(cfid->path);
	cfid->path = NULL;
	kfree(cfid);
}

static void cfid_release_ref(struct kref *ref)
{
	struct cached_fid *cfid = container_of(ref, struct cached_fid, refcount);

	__drop_cfid(cfid);
	call_rcu_hurry(&cfid->rcu, cfid_rcu_free);
}

bool drop_cached_dir(struct cached_fids *cfids, const void *key, int mode)
{
	struct cached_fid *cfid;

	if (!cfids || !key)
		return false;

	/*
	 * Raw lookup here as we _must_ find any matching cfid, no matter its state.
	 * Also, we might be racing with the SMB2 open in open_cached_dir(), so no need to wait
	 * for it to finish.
	 */
	cfid = find_cfid(cfids, key, mode, false);
	if (!cfid)
		return false;

	if (mode != CFID_LOOKUP_LEASEKEY) {
		drop_cfid(cfid);
	} else {
		/* we're locked in smb2_is_valid_lease_break(), so can't dput/close here */
		write_seqlock(&cfids->entries_seqlock);
		write_seqlock(&cfid->seqlock);
		invalidate_cfid(cfid);
		write_sequnlock(&cfid->seqlock);
		write_sequnlock(&cfids->entries_seqlock);

		/* put lookup ref */
		kref_put(&cfid->refcount, cfid_release_ref);
	}

	mod_delayed_work(cfid_put_wq, &cfids->laundromat_work, 0);

	return true;
}

void close_cached_dir(struct cached_fid *cfid)
{
	kref_put(&cfid->refcount, cfid_release_ref);
}

static void invalidate_all_cfids(struct cached_fids *cfids, bool closed)
{
	struct cached_fid *cfid;

	if (!cfids)
		return;

	/* mark all the cfids as closed and invalidate them for laundromat cleanup */
	write_seqlock(&cfids->entries_seqlock);
	list_for_each_entry(cfid, &cfids->entries, entry) {
		write_seqlock(&cfid->seqlock);
		invalidate_cfid(cfid);
		if (closed) {
			cfid->fid.persistent_fid = 0;
			cfid->fid.volatile_fid = 0;
		}
		write_sequnlock(&cfid->seqlock);
	}
	write_sequnlock(&cfids->entries_seqlock);

	/* run laundromat unconditionally now as there might have been previously queued work */
	mod_delayed_work(cfid_put_wq, &cfids->laundromat_work, 0);
}

/*
 * Called from cifs_kill_sb when we unmount a share
 */
void close_all_cached_dirs(struct cifs_sb_info *cifs_sb)
{
	struct rb_root *root = &cifs_sb->tlink_tree;
	struct rb_node *node;
	struct cifs_tcon *tcon;
	struct tcon_link *tlink;

	spin_lock(&cifs_sb->tlink_tree_lock);
	for (node = rb_first(root); node; node = rb_next(node)) {
		tlink = rb_entry(node, struct tcon_link, tl_rbnode);
		tcon = tlink_tcon(tlink);
		if (IS_ERR(tcon))
			continue;

		invalidate_all_cfids(tcon->cfids, false);
	}
	spin_unlock(&cifs_sb->tlink_tree_lock);

	/* Flush any pending work that will drop dentries */
	flush_workqueue(cfid_put_wq);
}

/*
 * Invalidate all cached dirs when a TCON has been reset
 * due to a session loss.
 */
void invalidate_all_cached_dirs(struct cached_fids *cfids)
{
	if (!cfids)
		return;

	synchronize_rcu();

	invalidate_all_cfids(cfids, true);
	flush_delayed_work(&cfids->laundromat_work);
}

static struct cached_fid *init_cached_dir(const char *path)
{
	struct cached_fid *cfid;

	cfid = kzalloc(sizeof(*cfid), GFP_ATOMIC);
	if (!cfid)
		return NULL;

	cfid->path = kstrdup(path, GFP_ATOMIC);
	if (!cfid->path) {
		kfree(cfid);
		return NULL;
	}

	INIT_LIST_HEAD(&cfid->entry);
	INIT_LIST_HEAD(&cfid->dirents.entries);
	mutex_init(&cfid->dirents.de_mutex);
	seqlock_init(&cfid->seqlock);

	/* this is our ref */
	kref_init(&cfid->refcount);

	/* this is caller/lease ref */
	kref_get(&cfid->refcount);

	/* initial cached dirents position */
	cfid->dirents.pos = 2;

	return cfid;
}

static void cfids_laundromat_worker(struct work_struct *work)
{
	unsigned int lseq, cseq, done = 1;
	struct cached_fid *cfid, *q;
	struct cached_fids *cfids;
	LIST_HEAD(entry);

	cfids = container_of(work, struct cached_fids, laundromat_work.work);

	rcu_read_lock();
	/* RCU-seqcounted read first so we avoid unnecessary seqcount invalidations */
	lseq = read_seqbegin(&cfids->entries_seqlock);
	list_for_each_entry_rcu(cfid, &cfids->entries, entry) {
		if (read_seqretry(&cfids->entries_seqlock, lseq)) {
			done = 0;
			break;
		}

		cseq = read_seqbegin(&cfid->seqlock);
		if (cfid_expired(cfid))
			done = 0;

		if (read_seqretry(&cfid->seqlock, cseq) || !done) {
			done = 0;
			break;
		}
	}

	/* no list invalidations and no invalidated/expired entries, requeue */
	if (!read_seqretry(&cfids->entries_seqlock, lseq) && done) {
		rcu_read_unlock();
		goto requeue;
	}
	rcu_read_unlock();

	/* now we know something was invalidated in the above read, do a write-locked run */
	write_seqlock(&cfids->entries_seqlock);
	list_for_each_entry_safe(cfid, q, &cfids->entries, entry) {
		write_seqlock(&cfid->seqlock);
		if (cfid_expired(cfid)) {
			//raw_write_seqcount_barrier(&cfids->entries_seqlock.seqcount);
			invalidate_cfid(cfid);
			list_move(&cfid->entry, &entry);
		}
		write_sequnlock(&cfid->seqlock);
	}
	write_sequnlock(&cfids->entries_seqlock);

	list_for_each_entry_safe(cfid, q, &entry, entry) {
		list_del(&cfid->entry);

		/*
		 * If a cfid reached here, we must cleanup anything unrelated to it, i.e. dentry and
		 * remote fid.
		 *
		 * For the cfid itself, we only drop our own ref (kref_init).  If there are still
		 * concurrent ref-holders, they'll drop it later (cfid is already invalid at this
		 * point, so can't be found anymore).
		 *
		 * No risk for a double list_del() here because cfid is only on this list now.
		 */
		drop_cfid(cfid);
	}
requeue:
	queue_delayed_work(cfid_put_wq, &cfids->laundromat_work, dir_cache_timeout * HZ);
}

struct cached_fids *init_cached_dirs(void)
{
	struct cached_fids *cfids;

	cfids = kzalloc(sizeof(*cfids), GFP_KERNEL);
	if (!cfids)
		return NULL;

	seqlock_init(&cfids->entries_seqlock);
	INIT_LIST_HEAD(&cfids->entries);

	INIT_DELAYED_WORK(&cfids->laundromat_work, cfids_laundromat_worker);
	queue_delayed_work(cfid_put_wq, &cfids->laundromat_work,
			   dir_cache_timeout * HZ);

	return cfids;
}
