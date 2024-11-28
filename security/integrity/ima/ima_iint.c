// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2008 IBM Corporation
 *
 * Authors:
 * Mimi Zohar <zohar@us.ibm.com>
 *
 * File: ima_iint.c
 *	- implements the IMA hook: ima_inode_free
 *	- cache integrity information in the inode security blob
 */
#include <linux/slab.h>

#include "ima.h"

static struct kmem_cache *ima_iint_cache __ro_after_init;

/**
 * ima_iint_find - Return the iint associated with an inode
 * @inode: Pointer to the inode
 *
 * Return the IMA integrity information (iint) associated with an inode, if the
 * inode was processed by IMA.
 *
 * Return: Found iint or NULL.
 */
struct ima_iint_cache *ima_iint_find(struct inode *inode)
{
	struct ima_iint_cache_lock *iint_lock;

	iint_lock = ima_inode_security(inode->i_security);
	if (!iint_lock)
		return NULL;

	return iint_lock->iint;
}

#define IMA_MAX_NESTING (FILESYSTEM_MAX_STACK_DEPTH + 1)

/*
 * It is not clear that IMA should be nested at all, but as long is it measures
 * files both on overlayfs and on underlying fs, we need to annotate the iint
 * mutex to avoid lockdep false positives related to IMA + overlayfs.
 * See ovl_lockdep_annotate_inode_mutex_key() for more details.
 */
static inline void ima_iint_lock_lockdep_annotate(struct mutex *mutex,
						  struct inode *inode)
{
#ifdef CONFIG_LOCKDEP
	static struct lock_class_key ima_iint_lock_mutex_key[IMA_MAX_NESTING];

	int depth = inode->i_sb->s_stack_depth;

	if (WARN_ON_ONCE(depth < 0 || depth >= IMA_MAX_NESTING))
		depth = 0;

	lockdep_set_class(mutex, &ima_iint_lock_mutex_key[depth]);
#endif
}

static void ima_iint_init_always(struct ima_iint_cache *iint,
				 struct inode *inode)
{
	iint->ima_hash = NULL;
	iint->real_inode.version = 0;
	iint->flags = 0UL;
	iint->atomic_flags = 0UL;
	iint->ima_file_status = INTEGRITY_UNKNOWN;
	iint->ima_mmap_status = INTEGRITY_UNKNOWN;
	iint->ima_bprm_status = INTEGRITY_UNKNOWN;
	iint->ima_read_status = INTEGRITY_UNKNOWN;
	iint->ima_creds_status = INTEGRITY_UNKNOWN;
	iint->measured_pcrs = 0;
}

static void ima_iint_free(struct ima_iint_cache *iint)
{
	kfree(iint->ima_hash);
	kmem_cache_free(ima_iint_cache, iint);
}

/**
 * ima_inode_get - Find or allocate an iint associated with an inode
 * @inode: Pointer to the inode
 *
 * Find an iint associated with an inode, and allocate a new one if not found.
 * Caller must lock i_mutex.
 *
 * Return: An iint on success, NULL on error.
 */
struct ima_iint_cache *ima_inode_get(struct inode *inode)
{
	struct ima_iint_cache_lock *iint_lock;
	struct ima_iint_cache *iint;

	iint_lock = ima_inode_security(inode->i_security);
	if (!iint_lock)
		return NULL;

	iint = iint_lock->iint;
	if (iint)
		return iint;

	iint = kmem_cache_alloc(ima_iint_cache, GFP_NOFS);
	if (!iint)
		return NULL;

	ima_iint_init_always(iint, inode);

	iint_lock->iint = iint;

	return iint;
}

/**
 * ima_inode_alloc_security - Called to init an inode
 * @inode: Pointer to the inode
 *
 * Initialize and annotate the mutex in the ima_iint_cache_lock structure.
 *
 * Return: Zero.
 */
int ima_inode_alloc_security(struct inode *inode)
{
	struct ima_iint_cache_lock *iint_lock;

	iint_lock = ima_inode_security(inode->i_security);

	mutex_init(&iint_lock->mutex);
	ima_iint_lock_lockdep_annotate(&iint_lock->mutex, inode);

	return 0;
}

/**
 * ima_inode_free_rcu - Called to free an inode via a RCU callback
 * @inode_security: The inode->i_security pointer
 *
 * Free the IMA data associated with an inode.
 */
void ima_inode_free_rcu(void *inode_security)
{
	struct ima_iint_cache_lock *iint_lock;

	iint_lock = ima_inode_security(inode_security);

	mutex_destroy(&iint_lock->mutex);

	if (iint_lock->iint)
		ima_iint_free(iint_lock->iint);
}

/**
 * ima_iint_lock - Lock integrity metadata
 * @inode: Pointer to the inode
 *
 * Lock integrity metadata.
 */
void ima_iint_lock(struct inode *inode)
{
	struct ima_iint_cache_lock *iint_lock;

	iint_lock = ima_inode_security(inode->i_security);

	/* Only inodes with i_security are processed by IMA. */
	if (iint_lock)
		mutex_lock(&iint_lock->mutex);
}

/**
 * ima_iint_unlock - Unlock integrity metadata
 * @inode: Pointer to the inode
 *
 * Unlock integrity metadata.
 */
void ima_iint_unlock(struct inode *inode)
{
	struct ima_iint_cache_lock *iint_lock;

	iint_lock = ima_inode_security(inode->i_security);

	/* Only inodes with i_security are processed by IMA. */
	if (iint_lock)
		mutex_unlock(&iint_lock->mutex);
}

static void ima_iint_init_once(void *foo)
{
	struct ima_iint_cache *iint = (struct ima_iint_cache *)foo;

	memset(iint, 0, sizeof(*iint));
}

void __init ima_iintcache_init(void)
{
	ima_iint_cache =
	    kmem_cache_create("ima_iint_cache", sizeof(struct ima_iint_cache),
			      0, SLAB_PANIC, ima_iint_init_once);
}
