// SPDX-License-Identifier: GPL-2.0
#include <linux/blkdev.h>
#include <linux/part_stat.h>
#include <linux/percpu.h>
#include <linux/seq_file.h>

#include "blk.h"

#define DISK_LAT_HIST_BOUNDS	24
#define DISK_LAT_HIST_BUCKETS	(DISK_LAT_HIST_BOUNDS + 1)

struct disk_lat_hist {
	u64 buckets[NR_STAT_GROUPS][DISK_LAT_HIST_BUCKETS];
};

static const u64 disk_lat_hist_bounds_us[DISK_LAT_HIST_BOUNDS] = {
	10, 20, 40, 80,
	100, 200, 400, 800,
	1000, 2000, 4000, 8000,
	10000, 20000, 40000, 80000,
	100000, 200000, 400000, 800000,
	1000000, 2000000, 4000000, 8000000,
};

static const int disk_lat_hist_order[NR_STAT_GROUPS] = {
	STAT_READ,
	STAT_WRITE,
	STAT_DISCARD,
	STAT_FLUSH,
};

void disk_lat_hist_alloc(struct block_device *bdev)
{
	/*
	 * Latency histograms are optional. If allocation fails,
	 * bd_lat_hist stays NULL; the record path skips histogram
	 * accounting and regular I/O statistics are unaffected.
	 */
	bdev->bd_lat_hist = alloc_percpu(struct disk_lat_hist);
	if (!bdev->bd_lat_hist)
		pr_warn_once("block: failed to allocate latency histograms\n");
}

void disk_lat_hist_free(struct block_device *bdev)
{
	if (!bdev->bd_lat_hist)
		return;
	free_percpu(bdev->bd_lat_hist);
	bdev->bd_lat_hist = NULL;
}

void disk_lat_hist_set_all(struct block_device *bdev, int value)
{
	int cpu;

	if (!bdev->bd_lat_hist)
		return;

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(bdev->bd_lat_hist, cpu), value,
		       sizeof(struct disk_lat_hist));
}

static void disk_lat_hist_record(struct block_device *bdev, int sgrp,
				 int bucket)
{
	if (!bdev || !bdev->bd_lat_hist)
		return;
	__this_cpu_inc(bdev->bd_lat_hist->buckets[sgrp][bucket]);
}

static int disk_lat_hist_bucket(u64 nsec)
{
	int low = 0, high = DISK_LAT_HIST_BOUNDS;

	while (low < high) {
		int mid = low + (high - low) / 2;

		if (nsec <= disk_lat_hist_bounds_us[mid] * NSEC_PER_USEC)
			high = mid;
		else
			low = mid + 1;
	}

	return low;
}

void disk_lat_hist_record_part(struct block_device *part, int sgrp, u64 nsec)
{
	struct block_device *whole;
	int bucket;

	if (sgrp < 0 || sgrp >= NR_STAT_GROUPS || !part || !part->bd_disk)
		return;

	bucket = disk_lat_hist_bucket(nsec);
	disk_lat_hist_record(part, sgrp, bucket);

	whole = bdev_whole(part);
	if (whole != part)
		disk_lat_hist_record(whole, sgrp, bucket);
}

static void disk_lat_hist_seq_show(struct seq_file *seqf,
				   struct block_device *bdev)
{
	u64 buckets[NR_STAT_GROUPS][DISK_LAT_HIST_BUCKETS] = { };
	int cpu, sgrp, i, bucket;

	if (!bdev->bd_lat_hist)
		return;

	for_each_possible_cpu(cpu) {
		struct disk_lat_hist *hist = per_cpu_ptr(bdev->bd_lat_hist, cpu);

		for (sgrp = 0; sgrp < NR_STAT_GROUPS; sgrp++)
			for (i = 0; i < DISK_LAT_HIST_BUCKETS; i++)
				buckets[sgrp][i] += hist->buckets[sgrp][i];
	}

	for (i = 0; i < NR_STAT_GROUPS; i++) {
		sgrp = disk_lat_hist_order[i];
		seq_printf(seqf, "%4d %7d %pg",
			   MAJOR(bdev->bd_dev), MINOR(bdev->bd_dev), bdev);
		for (bucket = 0; bucket < DISK_LAT_HIST_BUCKETS; bucket++)
			seq_printf(seqf, " %llu", buckets[sgrp][bucket]);
		seq_putc(seqf, '\n');
	}
}

int disk_lat_buckets_show(struct seq_file *seqf, void *v)
{
	int i;

	for (i = 0; i < DISK_LAT_HIST_BOUNDS; i++)
		seq_printf(seqf, "%s%llu", i ? " " : "",
			   disk_lat_hist_bounds_us[i]);
	seq_putc(seqf, '\n');

	return 0;
}

int disk_lat_hists_show(struct seq_file *seqf, void *v)
{
	struct gendisk *disk = v;
	struct block_device *part;
	unsigned long idx;

	rcu_read_lock();
	xa_for_each(&disk->part_tbl, idx, part) {
		if (bdev_is_partition(part) && !bdev_nr_sectors(part))
			continue;
		disk_lat_hist_seq_show(seqf, part);
	}
	rcu_read_unlock();

	return 0;
}
