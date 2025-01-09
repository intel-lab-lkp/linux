// SPDX-License-Identifier: GPL-2.0
//
// Register cache access API - flat caching support
//
// Copyright 2012 Wolfson Microelectronics plc
//
// Author: Mark Brown <broonie@opensource.wolfsonmicro.com>

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "internal.h"

static inline unsigned int regcache_flat_get_index(const struct regmap *map,
						   unsigned int reg)
{
	return regcache_get_index_by_order(map, reg);
}

struct regcache_flat_data {
	unsigned int *data;
	unsigned long *valid;
};

static int regcache_flat_init(struct regmap *map)
{
	int i;
	unsigned int cache_size;
	struct regcache_flat_data *cache = NULL;
	unsigned long *cache_valid = NULL;
	unsigned int *cache_data = NULL;

	if (!map || map->reg_stride_order < 0 || !map->max_register_is_set)
		return -EINVAL;

	cache_size = regcache_flat_get_index(map, map->max_register) + 1;
	cache_data = kcalloc(cache_size, sizeof(unsigned int), map->alloc_flags);
	if (!cache_data)
		return -ENOMEM;

	cache_valid = bitmap_zalloc(cache_size, map->alloc_flags);
	if (!cache_valid)
		goto err_free_valid;

	map->cache = kmalloc(sizeof(*cache), map->alloc_flags);
	if (!map->cache)
		goto err_free;

	cache = map->cache;
	cache->valid = cache_valid;
	cache->data = cache_data;

	for (i = 0; i < map->num_reg_defaults; i++) {
		unsigned int reg = map->reg_defaults[i].reg;
		unsigned int index = regcache_flat_get_index(map, reg);

		cache->data[index] = map->reg_defaults[i].def;
		__set_bit(index, cache->valid);
	}

	return 0;

err_free:
	kfree(cache_data);
err_free_valid:
	bitmap_free(cache_valid);
	return -ENOMEM;
}

static int regcache_flat_exit(struct regmap *map)
{
	struct regcache_flat_data *cache = map->cache;

	if (cache) {
		bitmap_free(cache->valid);
		kfree(cache->data);
	}
	kfree(cache);
	map->cache = NULL;

	return 0;
}

static int regcache_flat_read(struct regmap *map,
			      unsigned int reg, unsigned int *value)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int index = regcache_flat_get_index(map, reg);

	if (!test_bit(index, cache->valid))
		return -ENOENT;

	*value = cache->data[index];

	return 0;
}

static int regcache_flat_write(struct regmap *map, unsigned int reg,
			       unsigned int value)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int index = regcache_flat_get_index(map, reg);

	cache->data[index] = value;
	__set_bit(index, cache->valid);

	return 0;
}

static int regcache_flat_drop(struct regmap *map, unsigned int min,
			      unsigned int max)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int bitmap_min = regcache_flat_get_index(map, min);
	unsigned int bitmap_max = regcache_flat_get_index(map, max);

	bitmap_clear(cache->valid, index_min, bitmap_max + 1 - bitmap_min);

	return 0;
}

struct regcache_ops regcache_flat_ops = {
	.type = REGCACHE_FLAT,
	.name = "flat",
	.init = regcache_flat_init,
	.exit = regcache_flat_exit,
	.read = regcache_flat_read,
	.write = regcache_flat_write,
	.drop = regcache_flat_drop,
};
