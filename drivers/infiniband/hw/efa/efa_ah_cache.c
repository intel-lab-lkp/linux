// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright 2026 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <linux/slab.h>

#include "efa_ah_cache.h"

static const struct rhashtable_params ah_cache_params = {
	.key_len = sizeof(struct efa_ah_cache_key),
	.key_offset = offsetof(struct efa_ah_cache_entry, key),
	.head_offset = offsetof(struct efa_ah_cache_entry, linkage),
};

int efa_ah_cache_init(struct efa_ah_cache *ah_cache)
{
	int err;

	mutex_init(&ah_cache->lock);
	err = rhashtable_init(&ah_cache->hashtable, &ah_cache_params);
	if (err)
		mutex_destroy(&ah_cache->lock);

	return err;
}

static void efa_ah_cache_entry_free(void *ptr, void *arg)
{
	struct efa_ah_cache_entry *entry = ptr;

	mutex_destroy(&entry->lock);
	kfree(entry);
}

void efa_ah_cache_destroy(struct efa_ah_cache *ah_cache)
{
	rcu_barrier();
	rhashtable_free_and_destroy(&ah_cache->hashtable, efa_ah_cache_entry_free, NULL);
	mutex_destroy(&ah_cache->lock);
}

static struct efa_ah_cache_entry *efa_ah_cache_lookup(struct efa_ah_cache *ah_cache, u16 pd,
						      u8 *gid)
	__must_hold(&ah_cache->lock)
{
	struct efa_ah_cache_key key = {};

	memcpy(key.gid, gid, sizeof(key.gid));
	key.pd = pd;

	return rhashtable_lookup_fast(&ah_cache->hashtable, &key, ah_cache_params);
}

/**
 * efa_ah_cache_get_or_create - Get or create an AH cache entry
 * @ah_cache: AH cache
 * @pd: Protection domain number
 * @gid: GID address
 *
 * Look up an AH cache entry by PD and GID. If found, increment the refcount and
 * return it. If not found, allocate a new entry and insert it into the
 * hashtable. The entry is returned unlocked.
 *
 * Return: Pointer to the entry on success, ERR_PTR on failure.
 */
struct efa_ah_cache_entry *efa_ah_cache_get_or_create(struct efa_ah_cache *ah_cache, u16 pd,
						      u8 *gid)
{
	struct efa_ah_cache_entry *entry;
	int err;

	mutex_lock(&ah_cache->lock);

	entry = efa_ah_cache_lookup(ah_cache, pd, gid);
	if (entry) {
		refcount_inc(&entry->refcount);
		mutex_unlock(&ah_cache->lock);
		return entry;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&ah_cache->lock);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(entry->key.gid, gid, sizeof(entry->key.gid));
	entry->key.pd = pd;
	refcount_set(&entry->refcount, 1);
	mutex_init(&entry->lock);

	err = rhashtable_insert_fast(&ah_cache->hashtable, &entry->linkage, ah_cache_params);
	if (err) {
		mutex_destroy(&entry->lock);
		kfree(entry);
		mutex_unlock(&ah_cache->lock);
		return ERR_PTR(err);
	}

	mutex_unlock(&ah_cache->lock);
	return entry;
}

/**
 * efa_ah_cache_put_unless_last - Release a reference to an AH cache entry
 * @ah_cache: AH cache
 * @pd: Protection domain number
 * @gid: GID address
 *
 * If this is not the last reference, decrement the refcount and return NULL.
 * If this is the last reference, return the entry with its mutex locked
 * without decrementing.
 *
 * Return: Pointer to the locked entry if last reference, NULL otherwise.
 */
struct efa_ah_cache_entry *efa_ah_cache_put_unless_last(struct efa_ah_cache *ah_cache, u16 pd,
							u8 *gid)
{
	struct efa_ah_cache_entry *entry;

	mutex_lock(&ah_cache->lock);
	entry = efa_ah_cache_lookup(ah_cache, pd, gid);
	if (!entry) {
		mutex_unlock(&ah_cache->lock);
		return NULL;
	}

	if (refcount_dec_not_one(&entry->refcount)) {
		mutex_unlock(&ah_cache->lock);
		return NULL;
	}

	mutex_lock(&entry->lock);
	mutex_unlock(&ah_cache->lock);
	return entry;
}

/**
 * efa_ah_cache_put - Release the final reference to an AH cache entry
 * @ah_cache: AH cache
 * @entry: AH cache entry
 *
 * Decrement the refcount. If it reaches zero, the entry is removed from the
 * hashtable and freed. Otherwise, the entry is kept for reuse.
 *
 * Called after the device destroy completes or on a failed create to release
 * the caller's reference.
 */
void efa_ah_cache_put(struct efa_ah_cache *ah_cache, struct efa_ah_cache_entry *entry)
{
	mutex_lock(&ah_cache->lock);
	if (!refcount_dec_and_test(&entry->refcount)) {
		mutex_unlock(&ah_cache->lock);
		return;
	}

	rhashtable_remove_fast(&ah_cache->hashtable, &entry->linkage, ah_cache_params);
	mutex_unlock(&ah_cache->lock);

	mutex_destroy(&entry->lock);
	kfree_rcu(entry, rcu_head);
}
