// SPDX-License-Identifier: GPL-2.0-only
/* Benchmark bitmap, IDA and Maple Tree allocation of variable-sized regions. */

#include <linux/bitmap.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/maple_tree.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/xarray.h>

#define MAP_SIZE	(1000000UL)
#define REGION_MAX_SIZE	32

static DECLARE_BITMAP(alloc_bitmap, MAP_SIZE) __initdata;
/* One more request guarantees that even an all-ones trace reaches ENOSPC. */
static u8 region_sizes[MAP_SIZE + 1] __initdata;
static unsigned long region_indexes[MAP_SIZE] __initdata;

static unsigned long __init benchmark_bitmap(unsigned long capacity)
{
	unsigned long count, index;
	ktime_t alloc_time, free_time;
	size_t memory;

	bitmap_zero(alloc_bitmap, MAP_SIZE);
	alloc_time = ktime_get();
	for (count = 0; count < ARRAY_SIZE(region_sizes); count++) {
		index = bitmap_find_next_zero_area(alloc_bitmap, capacity, 0,
						   region_sizes[count], 0);
		if (index >= capacity)
			break;

		region_indexes[count] = index;
		bitmap_set(alloc_bitmap, index, region_sizes[count]);
	}
	alloc_time = ktime_get() - alloc_time;

	index = count;
	free_time = ktime_get();
	while (index--)
		bitmap_clear(alloc_bitmap, region_indexes[index],
			     region_sizes[index]);
	free_time = ktime_get() - free_time;
	memory = BITS_TO_LONGS(capacity) * sizeof(unsigned long);
	pr_err("%-6s   %12llu   %12llu   %8lu   %10zu\n", "bitmap", alloc_time,
		free_time, capacity, memory);

	return count;
}

static size_t __init ida_memory(unsigned long nr_ids)
{
	unsigned long entries = DIV_ROUND_UP(nr_ids, IDA_BITMAP_BITS);
	unsigned long bitmaps = nr_ids / IDA_BITMAP_BITS;
	unsigned long nodes = 0;

	if (nr_ids % IDA_BITMAP_BITS > BITS_PER_XA_VALUE)
		bitmaps++;

	while (entries > 1) {
		entries = DIV_ROUND_UP(entries, XA_CHUNK_SIZE);
		nodes += entries;
	}

	return sizeof(struct ida) + bitmaps * sizeof(struct ida_bitmap) +
		nodes * sizeof(struct xa_node);
}

static unsigned long __init benchmark_ida(unsigned long capacity)
{
	struct ida ida = IDA_INIT(ida);
	unsigned long count, index, offset, nr_ids = 0;
	ktime_t alloc_time, free_time;
	int id = -ENOSPC;

	alloc_time = ktime_get();
	for (count = 0; count < ARRAY_SIZE(region_sizes); count++) {
		for (offset = 0; offset < region_sizes[count]; offset++) {
			id = ida_alloc_max(&ida, capacity - 1, GFP_KERNEL);
			if (id < 0)
				break;
			if (!offset)
				region_indexes[count] = id;
		}
		if (id < 0) {
			while (offset--)
				ida_free(&ida, region_indexes[count] + offset);
			break;
		}
		WARN_ON(id != region_indexes[count] + region_sizes[count] - 1);
		nr_ids += region_sizes[count];
	}
	alloc_time = ktime_get() - alloc_time;
	WARN_ON(id != -ENOSPC);

	index = count;
	free_time = ktime_get();
	while (index--)
		for (offset = 0; offset < region_sizes[index]; offset++)
			ida_free(&ida, region_indexes[index] + offset);
	free_time = ktime_get() - free_time;
	pr_err("%-6s   %12llu   %12llu   %8lu   %10zu\n", "IDA", alloc_time,
		free_time, capacity, ida_memory(nr_ids));
	ida_destroy(&ida);

	return count;
}

static unsigned long __init benchmark_maple_tree(unsigned long capacity)
{
	struct maple_tree mt = MTREE_INIT(mt, MT_FLAGS_ALLOC_RANGE);
	unsigned long count, index;
	ktime_t alloc_time, free_time;
	size_t memory;
	int ret;

	alloc_time = ktime_get();
	for (count = 0; count < ARRAY_SIZE(region_sizes); count++) {
		ret = mtree_alloc_range(&mt, &index, xa_mk_value(count + 1),
					region_sizes[count], 0, capacity - 1,
					GFP_KERNEL);
		if (ret)
			break;

		region_indexes[count] = index;
	}
	alloc_time = ktime_get() - alloc_time;
	WARN_ON(ret != -EBUSY);

	index = count;
	free_time = ktime_get();
	while (index--)
		mtree_erase(&mt, region_indexes[index]);
	free_time = ktime_get() - free_time;
	/* Minimum storage assuming fully occupied allocation-range leaf nodes. */
	memory = sizeof(mt) + DIV_ROUND_UP(count, MAPLE_ARANGE64_SLOTS) *
		 sizeof(struct maple_node);
	pr_err("%-6s   %12llu   %12llu   %8lu   %10zu\n", "maple", alloc_time,
		free_time, capacity, memory);
	mtree_destroy(&mt);

	return count;
}

static int __init region_alloc_benchmark(void)
{
	static const unsigned long capacities[] = { 1000000, 100000, 10000, 1000 };
	unsigned long bitmap_count, ida_count, maple_count;
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(region_sizes); i++)
		region_sizes[i] = get_random_u32_below(REGION_MAX_SIZE) + 1;

	pr_err("\nStart testing bitmap vs IDA vs Maple Tree region allocation\n");
	pr_err("%-6s   %12s   %12s   %8s   %10s\n", "type", "alloc (ns)", "free (ns)",
		"capacity", "memory (B)");
	for (i = 0; i < ARRAY_SIZE(capacities); i++) {
		bitmap_count = benchmark_bitmap(capacities[i]);
		ida_count = benchmark_ida(capacities[i]);
		maple_count = benchmark_maple_tree(capacities[i]);
		WARN_ON(bitmap_count != ida_count);
		WARN_ON(bitmap_count != maple_count);
	}

	/* Let the benchmark be loaded and run repeatedly without rmmod. */
	return -EINVAL;
}
module_init(region_alloc_benchmark);

MODULE_AUTHOR("Yury Norov <ynorov@nvidia.com>");
MODULE_DESCRIPTION("Benchmark bitmap, IDA and Maple Tree region allocation");
MODULE_LICENSE("GPL");
