/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PART_STAT_H
#define _LINUX_PART_STAT_H

#include <linux/blkdev.h>
#include <asm/local.h>

/*
 * Diskstats latency histogram:
 * - Bucket upper bounds are power-of-two in usecs, starting at DISK_LAT_BASE_USEC.
 * - The last bucket is a saturation bucket for latencies >= DISK_LAT_MAX_USEC.
 *
 * Latency is tracked in NR_STAT_SLICES 1-second slices and
 * summed to compute a NR_STAT_SLICES-second P99 latency.
 */
#define NR_STAT_BUCKETS 21
#define NR_STAT_SLICES 5
#define DISK_LAT_BASE_USEC 8U
#define DISK_LAT_MAX_USEC (DISK_LAT_BASE_USEC << (NR_STAT_BUCKETS - 1))

struct disk_stats {
	u64 nsecs[NR_STAT_GROUPS];
	unsigned long sectors[NR_STAT_GROUPS];
	unsigned long ios[NR_STAT_GROUPS];
	unsigned long merges[NR_STAT_GROUPS];
	unsigned long io_ticks;
	local_t in_flight[2];
	u32 latency_epoch[NR_STAT_SLICES];
	u32 latency[NR_STAT_GROUPS][NR_STAT_SLICES][NR_STAT_BUCKETS];
};

/*
 * Macros to operate on percpu disk statistics:
 *
 * part_stat_{add|sub|inc|dec}() modify the stat counters and should
 * be called between part_stat_lock() and part_stat_unlock().
 *
 * part_stat_read() can be called at any time.
 */
#define part_stat_lock()	preempt_disable()
#define part_stat_unlock()	preempt_enable()

#define part_stat_get_cpu(part, field, cpu)				\
	(per_cpu_ptr((part)->bd_stats, (cpu))->field)

#define part_stat_get(part, field)					\
	part_stat_get_cpu(part, field, smp_processor_id())

#define part_stat_read(part, field)					\
({									\
	TYPEOF_UNQUAL((part)->bd_stats->field) res = 0;			\
	unsigned int _cpu;						\
	for_each_possible_cpu(_cpu)					\
		res += per_cpu_ptr((part)->bd_stats, _cpu)->field; \
	res;								\
})

static inline void part_stat_set_all(struct block_device *part, int value)
{
	int i;

	for_each_possible_cpu(i)
		memset(per_cpu_ptr(part->bd_stats, i), value,
				sizeof(struct disk_stats));
}

#define part_stat_read_accum(part, field)				\
	(part_stat_read(part, field[STAT_READ]) +			\
	 part_stat_read(part, field[STAT_WRITE]) +			\
	 part_stat_read(part, field[STAT_DISCARD]))

#define __part_stat_add(part, field, addnd)				\
	__this_cpu_add((part)->bd_stats->field, addnd)

#define part_stat_add(part, field, addnd)	do {			\
	__part_stat_add((part), field, addnd);				\
	if (bdev_is_partition(part))					\
		__part_stat_add(bdev_whole(part), field, addnd);	\
} while (0)

#define part_stat_dec(part, field)					\
	part_stat_add(part, field, -1)
#define part_stat_inc(part, field)					\
	part_stat_add(part, field, 1)
#define part_stat_sub(part, field, subnd)				\
	part_stat_add(part, field, -subnd)

#define part_stat_local_dec(part, field)				\
	local_dec(&(part_stat_get(part, field)))
#define part_stat_local_inc(part, field)				\
	local_inc(&(part_stat_get(part, field)))
#define part_stat_local_read(part, field)				\
	local_read(&(part_stat_get(part, field)))
#define part_stat_local_read_cpu(part, field, cpu)			\
	local_read(&(part_stat_get_cpu(part, field, cpu)))

unsigned int bdev_count_inflight(struct block_device *part);

static inline unsigned int diskstat_latency_bucket(u64 latency_ns)
{
	u64 latency_us = latency_ns / 1000;
	u64 scaled;

	if (latency_us <= DISK_LAT_BASE_USEC)
		return 0;

	if (latency_us >= DISK_LAT_MAX_USEC)
		return NR_STAT_BUCKETS - 1;

	scaled = div_u64(latency_us - 1, DISK_LAT_BASE_USEC);
	return min_t(unsigned int, (unsigned int)fls64(scaled),
			NR_STAT_BUCKETS - 1);
}

static inline u32 diskstat_latency_bucket_upper_us(unsigned int bucket)
{
	if (bucket >= NR_STAT_BUCKETS - 1)
		return DISK_LAT_MAX_USEC;
	return DISK_LAT_BASE_USEC << bucket;
}

static inline u32 diskstat_latency_bucket_us(unsigned int bucket)
{
	u32 high;
	u32 low;

	if (bucket >= NR_STAT_BUCKETS - 1)
		return DISK_LAT_MAX_USEC;

	high = diskstat_latency_bucket_upper_us(bucket);
	low = high >> 1;
	return low + (low >> 1);
}

static inline void __part_stat_latency_prepare(struct block_device *part,
		u32 epoch, unsigned int slice)
{
	struct disk_stats *stats = per_cpu_ptr(part->bd_stats, smp_processor_id());
	int group;

	if (likely(stats->latency_epoch[slice] == epoch))
		return;

	for (group = 0; group < NR_STAT_GROUPS; group++)
		memset(stats->latency[group][slice], 0,
				sizeof(stats->latency[group][slice]));
	stats->latency_epoch[slice] = epoch;
}

static inline void part_stat_latency_record(struct block_device *part,
		int sgrp, unsigned long now, unsigned int bucket)
{
	u32 epoch = now / HZ;
	unsigned int slice = epoch % NR_STAT_SLICES;

	__part_stat_latency_prepare(part, epoch, slice);
	if (bdev_is_partition(part))
		__part_stat_latency_prepare(bdev_whole(part), epoch, slice);

	part_stat_inc(part, latency[sgrp][slice][bucket]);
}

#endif /* _LINUX_PART_STAT_H */
