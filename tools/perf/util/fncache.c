// SPDX-License-Identifier: GPL-2.0-only
/* Manage a cache of file names' existence */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/compiler.h>
#include "fncache.h"
#include "hashmap.h"

static struct perf_hashmap *fncache;

static size_t fncache__hash(long key, void *ctx __maybe_unused)
{
	return str_hash((const char *)key);
}

static bool fncache__equal(long key1, long key2, void *ctx __maybe_unused)
{
	return strcmp((const char *)key1, (const char *)key2) == 0;
}

static void fncache__init(void)
{
	fncache = perf_hashmap__new(fncache__hash, fncache__equal, /*ctx=*/NULL);
}

static struct perf_hashmap *fncache__get(void)
{
	static pthread_once_t fncache_once = PTHREAD_ONCE_INIT;

	pthread_once(&fncache_once, fncache__init);

	return fncache;
}

static bool lookup_fncache(const char *name, bool *res)
{
	struct perf_hashmap *map = fncache__get();
	long val;

	if (!map || !perf_hashmap__find(map, name, &val))
		return false;

	*res = (val != 0);
	return true;
}

static void update_fncache(const char *name, bool res)
{
	struct perf_hashmap *map = fncache__get();
	char *old_key = NULL, *key = strdup(name);

	if (map && key) {
		perf_hashmap__set(map, key, res, &old_key, /*old_value*/NULL);
		free(old_key);
	} else {
		free(key);
	}
}

/* No LRU, only use when bounded in some other way. */
bool file_available(const char *name)
{
	bool res;

	if (lookup_fncache(name, &res))
		return res;
	res = access(name, R_OK) == 0;
	update_fncache(name, res);
	return res;
}
