/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef BRMR_SRV_H
#define BRMR_SRV_H

#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/radix-tree.h>

#include "brmr-proto.h"
#include "rmr-req.h"

#define BRMR_SERVER_VER_MAJOR 0
#define BRMR_SERVER_VER_MINOR 1

#ifndef BRMR_SERVER_VER_STRING
#define BRMR_SERVER_VER_STRING	__stringify(BRMR_SERVER_VER_MAJOR) "." \
				__stringify(BRMR_SERVER_VER_MINOR)
#endif

#define DEFAULT_BLK_OPEN_FLAGS (BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_EXCL)

#define BRMR_BLK_STORE_MAGIC	0xC0FFEE
#define BLK_STR_MD_SIZE		PAGE_SIZE
#define BLK_STR_MD_SIZE_SECTORS (PAGE_SIZE / SECTOR_SIZE)
#define BLK_STR_MIN_MAPPED_SIZE (PAGE_SIZE + BLK_STR_MD_SIZE)

extern struct list_head store_list;
extern struct mutex store_mutex;

extern struct rmr_srv_store_ops pstore_blk_ops;
extern struct kobject *rmr_strs_kobj;

/* brmr server */

enum brmr_srv_store_state {
	BRMR_SRV_STORE_OPEN,
	BRMR_SRV_STORE_MAPPED,
	BRMR_SRV_STORE_NEED_SYNC,
};

struct brmr_srv_io_priv {
	struct brmr_srv_blk_dev	*dev;
	void			*priv;
};

struct rmr_blk_dev_params {
	u32 max_hw_sectors;
	u32 max_write_zeroes_sectors;
	u32 max_discard_sectors;
	u32 discard_granularity;
	u32 discard_alignment;
	u16 physical_block_size;
	u16 logical_block_size;
	u16 max_segments;
	u16 secure_discard;
	u8 cache_policy;
};

struct brmr_srv_blk_dev {
	char poolname[NAME_MAX];
	struct block_device *bdev;
	struct file *bdev_file;
	struct list_head entry;
	char name[BDEVNAME_SIZE];
	struct rmr_pool *pool;
	u64 mapped_size;	/* in sectors */
	u64 dev_size;		/* in sectors */
	struct rmr_blk_dev_params dev_params;
	struct kmem_cache *io_priv_cache;
	struct kobject kobj;
	unsigned long state;
	struct completion comp;
	struct percpu_ref kref;
};

struct brmr_srv_blk_dev_meta {
	char poolname[NAME_MAX];
	struct rmr_blk_dev_params dev_params;
	u64 magic; /* magic token to identify a header */
	u32 version; /* version of the header itself */
	u64 dev_size;
	u64 mapped_size;
	u64 state;
	u64 offset;
	u64 ts;
} __packed;

int brmr_srv_blk_validate_md(struct brmr_srv_blk_dev *dev, struct brmr_srv_blk_dev_meta *meta);
struct brmr_srv_blk_dev *brmr_srv_blk_create(const char *path, char *name);
void brmr_srv_blk_destroy(struct brmr_srv_blk_dev *dev);
int brmr_srv_blk_open(struct brmr_srv_blk_dev *dev, const char *path, bool create, bool replace);
void brmr_srv_blk_close(struct brmr_srv_blk_dev *dev, bool delete);

int brmr_srv_read_and_check_md(struct brmr_srv_blk_dev *dev, void *md_page);

static inline void brmr_srv_blk_set_state(struct brmr_srv_blk_dev *dev,
					  enum brmr_srv_store_state state)
{
	set_bit(state, &dev->state);
}

static inline void brmr_srv_blk_clear_state(struct brmr_srv_blk_dev *dev,
					    enum brmr_srv_store_state state)
{
	clear_bit(state, &dev->state);
}

static inline int brmr_srv_blk_get_ref(struct brmr_srv_blk_dev *dev)
{
	return percpu_ref_tryget(&dev->kref);
}

static inline void brmr_srv_blk_put_ref(struct brmr_srv_blk_dev *dev)
{
	percpu_ref_put(&dev->kref);
}


/* brmr-server-sysfs.c */

int brmr_srv_create_sysfs_files(void);
void brmr_srv_destroy_sysfs_files(void);
void blk_str_destroy_sysfs_files(struct brmr_srv_blk_dev *dev,
				 const struct attribute *sysfs_self);

#endif /* BRMR_SRV_H */
