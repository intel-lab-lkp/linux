// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "peerlookup.h"
#include "linux/printk.h"
#include "linux/rcupdate.h"
#include "linux/rhashtable-types.h"
#include "linux/rhashtable.h"
#include "linux/siphash.h"
#include "messages.h"
#include "peer.h"
#include "noise.h"
#include "linux/memory.h"

static inline u32 index_hashfn(const void *data, u32 len, u32 seed)
{
	const u32 *index = data;
	return *index;
}

static const struct rhashtable_params index_ht_params = {
	.head_offset = offsetof(struct index_hashtable_entry, index_hash),
	.key_offset = offsetof(struct index_hashtable_entry, index),
	.hashfn = index_hashfn,
	.key_len = sizeof(__le32),
	.automatic_shrinking = true,
};

struct peer_hash_pubkey {
	siphash_key_t key;
	u8 pubkey[NOISE_PUBLIC_KEY_LEN];
};

static inline u32 wg_peer_obj_hashfn(const void *data, u32 len, u32 seed)
{
	const struct wg_peer *peer = data;
	struct peer_hash_pubkey key;
	u64 hash;

	memcpy(&key.key, &peer->handshake.hash_seed, sizeof(key.key));
	memcpy(&key.pubkey, &peer->handshake.remote_static, NOISE_PUBLIC_KEY_LEN);

	hash = siphash(&key.pubkey, NOISE_PUBLIC_KEY_LEN, &key.key);

	return (u32)hash;
}

static inline u32 wg_peer_hashfn(const void *data, u32 len, u32 seed)
{
	const struct peer_hash_pubkey *key = data;
	u64 hash = siphash(&key->pubkey, NOISE_PUBLIC_KEY_LEN, &key->key);

	return (u32)hash;
}

static inline int wg_peer_cmpfn(struct rhashtable_compare_arg *arg,
				const void *obj)
{
	const struct peer_hash_pubkey *key = arg->key;
	const struct wg_peer *peer = obj;

	return memcmp(key->pubkey, &peer->handshake.remote_static,
		      NOISE_PUBLIC_KEY_LEN);
}

static const struct rhashtable_params pubkey_ht_params = {
	.head_offset = offsetof(struct wg_peer, pubkey_hash),
	.key_offset = offsetof(struct wg_peer, handshake.remote_static),
	.obj_cmpfn = wg_peer_cmpfn,
	.obj_hashfn = wg_peer_obj_hashfn,
	.hashfn = wg_peer_hashfn,
	.automatic_shrinking = true,
};

struct pubkey_hashtable *wg_pubkey_hashtable_alloc(void)
{
	int ret;

	struct pubkey_hashtable *table = kvmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return NULL;

	get_random_bytes(&table->key, sizeof(table->key));
	ret = rhashtable_init(&table->rhashtable, &pubkey_ht_params);
	if (ret) {
		kvfree(table);
		return NULL;
	}
	mutex_init(&table->lock);
	return table;
}

void wg_pubkey_hashtable_add(struct pubkey_hashtable *table,
			     struct wg_peer *peer)
{
	struct peer_hash_pubkey key;

	mutex_lock(&table->lock);
	memcpy(&peer->handshake.hash_seed, &table->key,
	       sizeof(peer->handshake.hash_seed));
	memcpy(&key.key, &peer->handshake.hash_seed, sizeof(key.key));
	memcpy(&key.pubkey, peer->handshake.remote_static, NOISE_PUBLIC_KEY_LEN);

	rhashtable_lookup_insert_key(&table->rhashtable, &key,
				     &peer->pubkey_hash, pubkey_ht_params);
	mutex_unlock(&table->lock);
}

void wg_pubkey_hashtable_remove(struct pubkey_hashtable *table,
				struct wg_peer *peer)
{
	mutex_lock(&table->lock);
	rhashtable_remove_fast(&table->rhashtable, &peer->pubkey_hash,
			       pubkey_ht_params);
	mutex_unlock(&table->lock);
}

/* Returns a strong reference to a peer */
struct wg_peer *
wg_pubkey_hashtable_lookup(struct pubkey_hashtable *table,
			   const u8 pubkey[NOISE_PUBLIC_KEY_LEN])
{
	struct wg_peer *peer = NULL;
	struct peer_hash_pubkey key;

	rcu_read_lock_bh();
	memcpy(&key.key, &table->key, sizeof(key.key));
	memcpy(&key.pubkey, pubkey, NOISE_PUBLIC_KEY_LEN);
	peer = rhashtable_lookup_fast(&table->rhashtable, &key,
				      pubkey_ht_params);
	peer = wg_peer_get_maybe_zero(peer);
	rcu_read_unlock_bh();

	return peer;
}

struct index_hashtable *wg_index_hashtable_alloc(void)
{
	struct index_hashtable *table = kvmalloc(sizeof(*table), GFP_KERNEL);

	if (!table)
		return NULL;

	if (rhashtable_init(&table->rhashtable, &index_ht_params)) {
		kvfree(table);
		return NULL;
	}

	spin_lock_init(&table->lock);
	return table;
}

/* At the moment, we limit ourselves to 2^20 total peers, which generally might
 * amount to 2^20*3 items in this hashtable. The algorithm below works by
 * picking a random number and testing it. We can see that these limits mean we
 * usually succeed pretty quickly:
 *
 * >>> def calculation(tries, size):
 * ...     return (size / 2**32)**(tries - 1) *  (1 - (size / 2**32))
 * ...
 * >>> calculation(1, 2**20 * 3)
 * 0.999267578125
 * >>> calculation(2, 2**20 * 3)
 * 0.0007318854331970215
 * >>> calculation(3, 2**20 * 3)
 * 5.360489012673497e-07
 * >>> calculation(4, 2**20 * 3)
 * 3.9261394135792216e-10
 *
 * At the moment, we don't do any masking, so this algorithm isn't exactly
 * constant time in either the random guessing or in the hash list lookup. We
 * could require a minimum of 3 tries, which would successfully mask the
 * guessing. this would not, however, help with the growing hash lengths, which
 * is another thing to consider moving forward.
 */

__le32 wg_index_hashtable_insert(struct index_hashtable *table,
				 struct index_hashtable_entry *entry)
{
	spin_lock_bh(&table->lock);
	rhashtable_remove_fast(&table->rhashtable, &entry->index_hash,
			       index_ht_params);
	spin_unlock_bh(&table->lock);

	rcu_read_lock_bh();
	rcu_read_lock();

search_unused_slot:
	/* First we try to find an unused slot, randomly, while unlocked. */
	entry->index = (__force __le32)get_random_u32();
	if (rhashtable_lookup(&table->rhashtable, &entry->index,
			      index_ht_params)) {
		/* If it's already in use, we continue searching. */
		goto search_unused_slot;
	}

	/* Once we've found an unused slot, we lock it, and then double-check
	 * that nobody else stole it from us.
	 */
	spin_lock_bh(&table->lock);
	if (rhashtable_lookup(&table->rhashtable, &entry->index,
			      index_ht_params)) {
		spin_unlock_bh(&table->lock);
		/* If it was stolen, we start over. */
		goto search_unused_slot;
	}

	/* Otherwise, we know we have it exclusively (since we're locked),
	 * so we insert.
	 */
	rhashtable_insert_fast(&table->rhashtable, &entry->index_hash,
			       index_ht_params);
	spin_unlock_bh(&table->lock);

	rcu_read_unlock();
	rcu_read_unlock_bh();

	return entry->index;
}

bool wg_index_hashtable_replace(struct index_hashtable *table,
				struct index_hashtable_entry *old,
				struct index_hashtable_entry *new)
{
	bool ret;

	spin_lock_bh(&table->lock);
	ret = rhashtable_lookup_fast(&table->rhashtable, &old->index,
				     index_ht_params);
	if (unlikely(!ret))
		goto out;

	new->index = old->index;
	rhashtable_replace_fast(&table->rhashtable, &old->index_hash,
				&new->index_hash, index_ht_params);

out:
	spin_unlock_bh(&table->lock);
	return ret;
}

void wg_index_hashtable_remove(struct index_hashtable *table,
			       struct index_hashtable_entry *entry)
{
	spin_lock_bh(&table->lock);
	rhashtable_remove_fast(&table->rhashtable, &entry->index_hash,
			       index_ht_params);
	spin_unlock_bh(&table->lock);
}

/* Returns a strong reference to a entry->peer */
struct index_hashtable_entry *
wg_index_hashtable_lookup(struct index_hashtable *table,
			  const enum index_hashtable_type type_mask,
			  const __le32 index, struct wg_peer **peer)
{
	struct index_hashtable_entry *entry = NULL;

	rcu_read_lock_bh();
	entry = rhashtable_lookup_fast(&table->rhashtable, &index, index_ht_params);

	if (unlikely(!entry)) {
		rcu_read_unlock_bh();
		return entry;
	}

	if (likely(entry && (entry->type & type_mask))) {
		entry->peer = wg_peer_get_maybe_zero(entry->peer);
		if (likely(entry->peer))
			*peer = entry->peer;
		else
			entry = NULL;
	}

	rcu_read_unlock_bh();
	return entry;
}
