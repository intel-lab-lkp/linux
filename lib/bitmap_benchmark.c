// SPDX-License-Identifier: GPL-2.0-only
/*
 * Benchmarks for bitmap API.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/printk.h>

/*
 * Test bitmap should be big enough to include the cases when start is not in
 * the first word, and start+nbits lands in the following word.
 */
#define TEST_BIT_LEN (1000)

typedef void (*bitmap_bench_fn)(unsigned long *bitmap, unsigned long i, unsigned long nbits);

static void __init bench_bitmap(bitmap_bench_fn bench_fn, const char *name)
{
	DECLARE_BITMAP(bitmap, TEST_BIT_LEN);
	unsigned int cnt, nbits, i;
	ktime_t time;

	bitmap_fill(bitmap, TEST_BIT_LEN);
	time = ktime_get();
	for (cnt = 0; cnt < 5; cnt++) {
		for (nbits = 1; nbits <= BITS_PER_LONG; nbits++) {
			for (i = 0; i < TEST_BIT_LEN; i++) {
				if (i + nbits > TEST_BIT_LEN)
					break;
				bench_fn(bitmap, i, nbits);
			}
		}
	}
	time = ktime_get() - time;
	pr_info("Time spent in %s:\t%llu\n", name, time);
}

#undef TEST_BIT_LEN

static inline void bitmap_read_bench(unsigned long *bitmap, unsigned long i, unsigned long nbits)
{
	unsigned long val;
	/*
	 * Prevent the compiler from optimizing away the
	 * bitmap_read() by using its value.
	 */
	WRITE_ONCE(val, bitmap_read(bitmap, i, nbits));
}

static void __init test_bitmap_read_perf(void)
{
	bench_bitmap(bitmap_read_bench, __func__);
}

static inline void bitmap_write_bench(unsigned long *bitmap, unsigned long i, unsigned long nbits)
{
	unsigned long val = 0xfeedface;

	bitmap_write(bitmap, val, i, nbits);
}

static void __init test_bitmap_write_perf(void)
{
	bench_bitmap(bitmap_write_bench, __func__);
}

static int __init bitmap_benchmark_init(void)
{
	test_bitmap_read_perf();
	test_bitmap_write_perf();

	return 0;
}
module_init(bitmap_benchmark_init);

static void __exit bitmap_benchmark_exit(void)
{
	pr_info("Unloaded\n");
}
module_exit(bitmap_benchmark_exit);

MODULE_AUTHOR("Alexander Potapenko <glider@google.com>");
MODULE_DESCRIPTION("Benchmarks for bitmap API");
MODULE_LICENSE("GPL");
