// SPDX-License-Identifier: GPL-2.0
/*
 * Superblock watch for automatic NFSv4 state revocation on unmount.
 *
 * When a local filesystem is unmounted while NFS clients hold state,
 * this code automatically revokes that state so the unmount can proceed.
 *
 * Copyright (C) 2025 Oracle. All rights reserved.
 *
 * Author: Chuck Lever <chuck.lever@oracle.com>
 */

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/sunrpc/svc.h>
#include <linux/lockd/lockd.h>

#include "nfsd.h"
#include "netns.h"
#include "state.h"
#include "filecache.h"

#define NFSDDBG_FACILITY	NFSDDBG_PROC

static struct workqueue_struct *nfsd_sb_watch_wq;

/* Interval for progress warnings during unmount (in seconds) */
#define NFSD_STATE_REVOKE_INTERVAL	30

/*
 * Watch record for a superblock with NFS state. When the filesystem
 * is unmounted, the notifier callback finds this record and triggers
 * state revocation.
 */
struct nfsd_sb_watch {
	struct super_block	*sb;
	struct net		*net;
	struct work_struct	work;
	struct completion	done;
	struct rcu_head		rcu;
};

static void nfsd_sb_watch_free_rcu(struct rcu_head *rcu)
{
	struct nfsd_sb_watch *watch = container_of(rcu, struct nfsd_sb_watch, rcu);

	put_net(watch->net);
	kfree(watch);
}

/*
 * Work function for nfsd_sb_watch - runs in process context.
 * Cancels async COPYs, releases NLM locks, revokes NFSv4 state, and closes
 * cached NFSv2/3 files for the superblock.
 */
static void nfsd_sb_revoke_work(struct work_struct *work)
{
	struct nfsd_sb_watch *watch = container_of(work, struct nfsd_sb_watch, work);
	struct nfsd_net *nn = net_generic(watch->net, nfsd_net_id);

	pr_info("nfsd: unmount of %s, revoking NFS state\n", watch->sb->s_id);

	nfsd4_cancel_copy_by_sb(watch->net, watch->sb);
	/* Errors are logged by lockd; no recovery is possible. */
	(void)nlmsvc_unlock_all_by_sb(watch->sb);
	nfsd4_revoke_states(nn, watch->sb);
	nfsd_file_close_sb(watch->sb);

	pr_info("nfsd: state revocation for %s complete\n", watch->sb->s_id);

	complete(&watch->done);
	call_rcu(&watch->rcu, nfsd_sb_watch_free_rcu);
}

/*
 * Trigger state revocation for a superblock and wait for completion.
 *
 * The xa_erase() ensures exactly one path (either this notification or
 * NFSD shutdown) handles cleanup for a given watch record.
 */
static void nfsd_sb_trigger_revoke(struct nfsd_net *nn, struct super_block *sb)
{
	struct nfsd_sb_watch *watch;
	unsigned int elapsed = 0;
	long ret;

	watch = xa_erase(&nn->nfsd_sb_watches, (unsigned long)sb);
	if (!watch)
		return;

	queue_work(nfsd_sb_watch_wq, &watch->work);

	/*
	 * Block until state revocation completes. Periodic warnings help
	 * diagnose stuck operations (e.g., re-exports of an NFS mount
	 * whose backend server is unresponsive).
	 *
	 * The work function handles freeing, so this function can return
	 * early on interrupt. Open files keep the superblock alive until
	 * revocation closes them.
	 */
	for (;;) {
		ret = wait_for_completion_interruptible_timeout(&watch->done,
						NFSD_STATE_REVOKE_INTERVAL * HZ);
		if (ret > 0)
			return;

		if (ret == -ERESTARTSYS) {
			/*
			 * Interrupted by signal. If the work has not yet
			 * started, cancel it and run in this context: a
			 * successful cancel_work() means no other context
			 * will execute the work function, so it must run
			 * here to ensure state revocation occurs.
			 *
			 * If already running, cancel_work() waits for
			 * completion before returning false.
			 */
			if (cancel_work(&watch->work)) {
				pr_warn("nfsd: unmount of %s interrupted, revoking state in unmount context\n",
					sb->s_id);
				nfsd_sb_revoke_work(&watch->work);
				return;
			}
			pr_warn("nfsd: unmount of %s interrupted; revocation continues in background\n",
				sb->s_id);
			return;
		}

		/* Timed out - print warning and continue waiting */
		elapsed += NFSD_STATE_REVOKE_INTERVAL;
		pr_warn("nfsd: unmount of %s blocked for %u seconds waiting for NFS state revocation\n",
			sb->s_id, elapsed);
	}
}

/*
 * Notifier callback invoked when any filesystem is unmounted.
 * Check if this superblock is being watched and trigger revocation.
 */
static int nfsd_umount_notifier_call(struct notifier_block *nb,
				     unsigned long action, void *data)
{
	struct nfsd_net *nn = container_of(nb, struct nfsd_net, nfsd_umount_notifier);
	struct super_block *sb = data;

	nfsd_sb_trigger_revoke(nn, sb);
	return NOTIFY_DONE;
}

/**
 * nfsd_sb_watch - watch a superblock for unmount to trigger state revocation
 * @net: network namespace
 * @mnt: vfsmount for the filesystem
 *
 * When NFS state is created for a file on this filesystem, register a
 * watch so the umount notifier can revoke that state on unmount.
 * Returns nfs_ok on success, or an NFS error on failure.
 *
 * This function is idempotent - if a watch already exists for the
 * superblock, no new watch is created.
 */
__be32 nfsd_sb_watch(struct net *net, struct vfsmount *mnt)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct super_block *sb = mnt->mnt_sb;
	struct nfsd_sb_watch *new;
	int ret;

	if (xa_load(&nn->nfsd_sb_watches, (unsigned long)sb))
		return nfs_ok;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return nfserr_jukebox;

	new->sb = sb;
	new->net = get_net(net);
	INIT_WORK(&new->work, nfsd_sb_revoke_work);
	init_completion(&new->done);

	ret = xa_insert(&nn->nfsd_sb_watches, (unsigned long)sb, new, GFP_KERNEL);
	if (ret) {
		/*
		 * Another task beat us to it. Even if the winner has not
		 * yet completed insertion, returning here is safe: the
		 * caller holds an open file reference that prevents
		 * unmount from completing until state creation finishes.
		 */
		put_net(new->net);
		kfree(new);
		return nfs_ok;
	}

	/*
	 * Callers hold an open file reference, so unmount cannot clear
	 * SB_ACTIVE while this function executes. Warn if this assumption
	 * is violated, but handle it gracefully by cleaning up and
	 * returning an error.
	 */
	if (WARN_ON_ONCE(!(READ_ONCE(sb->s_flags) & SB_ACTIVE))) {
		new = xa_erase(&nn->nfsd_sb_watches, (unsigned long)sb);
		if (new) {
			put_net(new->net);
			kfree(new);
		}
		return nfserr_stale;
	}

	return nfs_ok;
}

/**
 * nfsd_sb_watch_setup - initialize umount watch for a network namespace
 * @nn: nfsd_net for this network namespace
 *
 * Called during nfs4_state_create_net(). Registers with the VFS umount
 * notifier chain to receive callbacks when filesystems are unmounted.
 */
int nfsd_sb_watch_setup(struct nfsd_net *nn)
{
	xa_init(&nn->nfsd_sb_watches);
	nn->nfsd_umount_notifier.notifier_call = nfsd_umount_notifier_call;
	return umount_register_notifier(&nn->nfsd_umount_notifier);
}

/*
 * Clean up all watch records during NFSD shutdown.
 *
 * xa_erase() synchronizes with nfsd_sb_trigger_revoke(): the path that
 * successfully erases an xarray entry performs cleanup for that entry.
 * A NULL return indicates the umount notification path is handling cleanup.
 */
static void nfsd_sb_watches_destroy(struct nfsd_net *nn)
{
	struct nfsd_sb_watch *watch;
	unsigned long index;

	xa_for_each(&nn->nfsd_sb_watches, index, watch) {
		watch = xa_erase(&nn->nfsd_sb_watches, index);
		if (!watch)
			continue; /* Umount notification path handling this */
		cancel_work_sync(&watch->work);
		put_net(watch->net);
		kfree(watch);
	}
	xa_destroy(&nn->nfsd_sb_watches);
}

/**
 * nfsd_sb_watch_shutdown - shutdown umount watch for a network namespace
 * @nn: nfsd_net for this network namespace
 *
 * Must be called during nfsd shutdown before tearing down client state.
 */
void nfsd_sb_watch_shutdown(struct nfsd_net *nn)
{
	umount_unregister_notifier(&nn->nfsd_umount_notifier);
	nfsd_sb_watches_destroy(nn);
	/*
	 * Ensure any work items still running complete before shutdown
	 * proceeds. This handles the case where an unmount notification
	 * returned early due to signal interruption but the work function
	 * is still executing nfsd_file_close_sb(). Without this flush,
	 * nfsd_file_cache_shutdown() could destroy the slab while the
	 * work function is still disposing file cache entries.
	 */
	flush_workqueue(nfsd_sb_watch_wq);
}

int nfsd_sb_watch_init(void)
{
	nfsd_sb_watch_wq = alloc_workqueue("nfsd_sb_watch", WQ_UNBOUND, 0);
	if (!nfsd_sb_watch_wq)
		return -ENOMEM;
	return 0;
}

void nfsd_sb_watch_exit(void)
{
	destroy_workqueue(nfsd_sb_watch_wq);
}
