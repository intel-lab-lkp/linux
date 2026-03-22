// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

/*
 * Generic non-thread safe hash map implementation.
 *
 * Copyright (c) 2019 Facebook
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <linux/err.h>
#include "hashmap.h"

/* make sure libbpf doesn't use kernel-only integer typedefs */
#pragma GCC poison u8 u16 u32 u64 s8 s16 s32 s64

/* prevent accidental re-addition of reallocarray() */
#pragma GCC poison reallocarray

/* start with 4 buckets */
#define PERF_HASHMAP_MIN_CAP_BITS 2

static void perf_hashmap_add_entry(struct perf_hashmap_entry **pprev,
			      struct perf_hashmap_entry *entry)
{
	entry->next = *pprev;
	*pprev = entry;
}

static void perf_hashmap_del_entry(struct perf_hashmap_entry **pprev,
			      struct perf_hashmap_entry *entry)
{
	*pprev = entry->next;
	entry->next = NULL;
}

void perf_hashmap__init(struct perf_hashmap *map, perf_hashmap_hash_fn hash_fn,
		   perf_hashmap_equal_fn equal_fn, void *ctx)
{
	map->hash_fn = hash_fn;
	map->equal_fn = equal_fn;
	map->ctx = ctx;

	map->buckets = NULL;
	map->cap = 0;
	map->cap_bits = 0;
	map->sz = 0;
}

struct perf_hashmap *perf_hashmap__new(perf_hashmap_hash_fn hash_fn,
			     perf_hashmap_equal_fn equal_fn,
			     void *ctx)
{
	struct perf_hashmap *map = malloc(sizeof(struct perf_hashmap));

	if (!map)
		return ERR_PTR(-ENOMEM);
	perf_hashmap__init(map, hash_fn, equal_fn, ctx);
	return map;
}

void perf_hashmap__clear(struct perf_hashmap *map)
{
	struct perf_hashmap_entry *cur, *tmp;
	size_t bkt;

	perf_hashmap__for_each_entry_safe(map, cur, tmp, bkt) {
		free(cur);
	}
	free(map->buckets);
	map->buckets = NULL;
	map->cap = map->cap_bits = map->sz = 0;
}

void perf_hashmap__free(struct perf_hashmap *map)
{
	if (IS_ERR_OR_NULL(map))
		return;

	perf_hashmap__clear(map);
	free(map);
}

size_t perf_hashmap__size(const struct perf_hashmap *map)
{
	return map->sz;
}

size_t perf_hashmap__capacity(const struct perf_hashmap *map)
{
	return map->cap;
}

static bool perf_hashmap_needs_to_grow(struct perf_hashmap *map)
{
	/* grow if empty or more than 75% filled */
	return (map->cap == 0) || ((map->sz + 1) * 4 / 3 > map->cap);
}

static int perf_hashmap_grow(struct perf_hashmap *map)
{
	struct perf_hashmap_entry **new_buckets;
	struct perf_hashmap_entry *cur, *tmp;
	size_t new_cap_bits, new_cap;
	size_t h, bkt;

	new_cap_bits = map->cap_bits + 1;
	if (new_cap_bits < PERF_HASHMAP_MIN_CAP_BITS)
		new_cap_bits = PERF_HASHMAP_MIN_CAP_BITS;

	new_cap = 1UL << new_cap_bits;
	new_buckets = calloc(new_cap, sizeof(new_buckets[0]));
	if (!new_buckets)
		return -ENOMEM;

	perf_hashmap__for_each_entry_safe(map, cur, tmp, bkt) {
		h = hash_bits(map->hash_fn(cur->key, map->ctx), new_cap_bits);
		perf_hashmap_add_entry(&new_buckets[h], cur);
	}

	map->cap = new_cap;
	map->cap_bits = new_cap_bits;
	free(map->buckets);
	map->buckets = new_buckets;

	return 0;
}

static bool perf_hashmap_find_entry(const struct perf_hashmap *map,
			       const long key, size_t hash,
			       struct perf_hashmap_entry ***pprev,
			       struct perf_hashmap_entry **entry)
{
	struct perf_hashmap_entry *cur, **prev_ptr;

	if (!map->buckets)
		return false;

	for (prev_ptr = &map->buckets[hash], cur = *prev_ptr;
	     cur;
	     prev_ptr = &cur->next, cur = cur->next) {
		if (map->equal_fn(cur->key, key, map->ctx)) {
			if (pprev)
				*pprev = prev_ptr;
			*entry = cur;
			return true;
		}
	}

	return false;
}

int perf_hashmap_insert(struct perf_hashmap *map, long key, long value,
		   enum perf_hashmap_insert_strategy strategy,
		   void *old_key, void *old_value)
{
	struct perf_hashmap_entry *entry;
	size_t h;
	int err;

	if (old_key)
		memset(old_key, 0, sizeof(long));
	if (old_value)
		memset(old_value, 0, sizeof(long));

	h = hash_bits(map->hash_fn(key, map->ctx), map->cap_bits);
	if (strategy != PERF_HASHMAP_APPEND &&
	    perf_hashmap_find_entry(map, key, h, NULL, &entry)) {
		if (old_key)
			memcpy(old_key, &entry->key, sizeof(long));
		if (old_value)
			memcpy(old_value, &entry->value, sizeof(long));

		if (strategy == PERF_HASHMAP_SET || strategy == PERF_HASHMAP_UPDATE) {
			entry->key = key;
			entry->value = value;
			return 0;
		} else if (strategy == PERF_HASHMAP_ADD) {
			return -EEXIST;
		}
	}

	if (strategy == PERF_HASHMAP_UPDATE)
		return -ENOENT;

	if (perf_hashmap_needs_to_grow(map)) {
		err = perf_hashmap_grow(map);
		if (err)
			return err;
		h = hash_bits(map->hash_fn(key, map->ctx), map->cap_bits);
	}

	entry = malloc(sizeof(struct perf_hashmap_entry));
	if (!entry)
		return -ENOMEM;

	entry->key = key;
	entry->value = value;
	perf_hashmap_add_entry(&map->buckets[h], entry);
	map->sz++;

	return 0;
}

bool perf_hashmap_find(const struct perf_hashmap *map, long key, void *value)
{
	struct perf_hashmap_entry *entry;
	size_t h;

	h = hash_bits(map->hash_fn(key, map->ctx), map->cap_bits);
	if (!perf_hashmap_find_entry(map, key, h, NULL, &entry))
		return false;

	if (value)
		memcpy(value, &entry->value, sizeof(long));
	return true;
}

bool perf_hashmap_delete(struct perf_hashmap *map, long key,
		    void *old_key, void *old_value)
{
	struct perf_hashmap_entry **pprev, *entry;
	size_t h;

	h = hash_bits(map->hash_fn(key, map->ctx), map->cap_bits);
	if (!perf_hashmap_find_entry(map, key, h, &pprev, &entry))
		return false;

	if (old_key)
		memcpy(old_key, &entry->key, sizeof(long));
	if (old_value)
		memcpy(old_value, &entry->value, sizeof(long));

	perf_hashmap_del_entry(pprev, entry);
	free(entry);
	map->sz--;

	return true;
}
