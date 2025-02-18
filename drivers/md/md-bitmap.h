/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bitmap.h: Copyright (C) Peter T. Breuer (ptb@ot.uc3m.es) 2003
 *
 * additions: Copyright (C) 2003-2004, Paul Clements, SteelEye Technology, Inc.
 */
#ifndef BITMAP_H
#define BITMAP_H 1

#include <linux/raid/md_p.h>

typedef __u16 bitmap_counter_t;
#define COUNTER_BITS 16
#define COUNTER_BIT_SHIFT 4
#define COUNTER_BYTE_SHIFT (COUNTER_BIT_SHIFT - 3)

#define NEEDED_MASK ((bitmap_counter_t) (1 << (COUNTER_BITS - 1)))
#define RESYNC_MASK ((bitmap_counter_t) (1 << (COUNTER_BITS - 2)))
#define COUNTER_MAX ((bitmap_counter_t) RESYNC_MASK - 1)

struct md_bitmap_stats {
	u64		events_cleared;
	int		behind_writes;
	bool		behind_wait;

	unsigned long	missing_pages;
	unsigned long	file_pages;
	unsigned long	sync_size;
	unsigned long	pages;
	struct file	*file;
};

struct bitmap_operations {
	bool (*enabled)(struct mddev *mddev);
	int (*create)(struct mddev *mddev, int slot);
	int (*resize)(struct mddev *mddev, sector_t blocks, int chunksize,
		      bool init);

	int (*load)(struct mddev *mddev);
	void (*destroy)(struct mddev *mddev);
	void (*flush)(struct mddev *mddev);
	void (*write_all)(struct mddev *mddev);
	void (*dirty_bits)(struct mddev *mddev, unsigned long s,
			   unsigned long e);
	void (*unplug)(struct mddev *mddev, bool sync);
	void (*daemon_work)(struct mddev *mddev);

	void (*start_behind_write)(struct mddev *mddev);
	void (*end_behind_write)(struct mddev *mddev);
	void (*wait_behind_writes)(struct mddev *mddev);

	int (*startwrite)(struct mddev *mddev, sector_t offset,
			  unsigned long sectors);
	void (*endwrite)(struct mddev *mddev, sector_t offset,
			 unsigned long sectors);
	bool (*start_sync)(struct mddev *mddev, sector_t offset,
			   sector_t *blocks, bool degraded);
	void (*end_sync)(struct mddev *mddev, sector_t offset, sector_t *blocks);
	void (*cond_end_sync)(struct mddev *mddev, sector_t sector, bool force);
	void (*close_sync)(struct mddev *mddev);

	void (*update_sb)(void *data);
	int (*get_stats)(void *data, struct md_bitmap_stats *stats);

	void (*sync_with_cluster)(struct mddev *mddev,
				  sector_t old_lo, sector_t old_hi,
				  sector_t new_lo, sector_t new_hi);
	void *(*get_from_slot)(struct mddev *mddev, int slot);
	int (*copy_from_slot)(struct mddev *mddev, int slot, sector_t *lo,
			      sector_t *hi, bool clear_bits);
	void (*set_pages)(void *data, unsigned long pages);
	void (*free)(void *data);
};

/* the bitmap API */
void mddev_set_bitmap_ops(struct mddev *mddev);

#endif
