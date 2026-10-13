// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, 2023 Linaro Limited
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include "optee_private.h"

struct optee_shm_handle_map_entry {
	struct tee_shm *shm;
	u64 global_id;
	struct rhash_head node;
};

static const struct rhashtable_params shm_handle_map_params = {
	.head_offset = offsetof(struct optee_shm_handle_map_entry, node),
	.key_offset = offsetof(struct optee_shm_handle_map_entry, global_id),
	.key_len = sizeof(u64),
	.automatic_shrinking = true,
};

static void shm_handle_map_free(void *ptr, void *arg)
{
	kfree(ptr);
}

/**
 * optee_shm_handle_map_init() - Initialize a shared-memory identity map
 * @map: Map to initialize
 *
 * Return: 0 on success or a negative error.
 */
int optee_shm_handle_map_init(struct optee_shm_handle_map *map)
{
	int rc;

	mutex_init(&map->mutex);
	rc = rhashtable_init(&map->rht, &shm_handle_map_params);
	if (rc)
		mutex_destroy(&map->mutex);

	return rc;
}

/**
 * optee_shm_handle_map_uninit() - Free a shared-memory identity map
 * @map: Initialized map with no concurrent users
 *
 * Free tracking entries, not SHM objects or their secure-world registrations.
 */
void optee_shm_handle_map_uninit(struct optee_shm_handle_map *map)
{
	rhashtable_free_and_destroy(&map->rht, shm_handle_map_free, NULL);
	mutex_destroy(&map->mutex);
}

/**
 * optee_shm_handle_map_add() - Track a registered shared-memory object
 * @map: Initialized map
 * @shm: Shared-memory object kept alive by its owner
 * @global_id: Opaque secure-world identity
 *
 * Return: 0 on success or a negative error.
 */
int optee_shm_handle_map_add(struct optee_shm_handle_map *map,
			     struct tee_shm *shm, u64 global_id)
{
	struct optee_shm_handle_map_entry *entry;
	int rc;

	entry = kmalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;

	entry->shm = shm;
	entry->global_id = global_id;

	mutex_lock(&map->mutex);
	rc = rhashtable_lookup_insert_fast(&map->rht, &entry->node,
					   shm_handle_map_params);
	mutex_unlock(&map->mutex);
	if (rc)
		kfree(entry);

	return rc;
}

/**
 * optee_shm_handle_map_remove() - Remove a shared-memory tracking entry
 * @map: Initialized map
 * @global_id: Opaque secure-world identity
 *
 * Return: 0 on success, -ENOENT if absent, or a negative removal error.
 */
int optee_shm_handle_map_remove(struct optee_shm_handle_map *map, u64 global_id)
{
	struct optee_shm_handle_map_entry *entry;
	int rc = -ENOENT;

	mutex_lock(&map->mutex);
	entry = rhashtable_lookup_fast(&map->rht, &global_id,
				       shm_handle_map_params);
	if (entry)
		rc = rhashtable_remove_fast(&map->rht, &entry->node,
					    shm_handle_map_params);
	mutex_unlock(&map->mutex);
	if (!rc)
		kfree(entry);

	return rc;
}

/**
 * optee_shm_handle_map_lookup() - Look up a registered shared-memory object
 * @map: Initialized map
 * @global_id: Opaque secure-world identity
 *
 * The caller must keep the SHM object alive; no reference is acquired here.
 *
 * Return: The shared-memory object or NULL if the identity is absent.
 */
struct tee_shm *
optee_shm_handle_map_lookup(struct optee_shm_handle_map *map, u64 global_id)
{
	struct optee_shm_handle_map_entry *entry;
	struct tee_shm *shm = NULL;

	mutex_lock(&map->mutex);
	entry = rhashtable_lookup_fast(&map->rht, &global_id,
				       shm_handle_map_params);
	if (entry)
		shm = entry->shm;
	mutex_unlock(&map->mutex);

	return shm;
}
