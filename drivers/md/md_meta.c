// SPDX-License-Identifier: GPL-2.0-or-later
#include "md.h"
#include "md-meta.h"

static void meta_end_write(struct bio *bio)
{
	struct bio *parent = bio->bi_private;

	if (!bio->bi_status)
		WRITE_ONCE(parent->bi_status, BLK_STS_OK);
	else
		pr_err("TODO: md_error\n");

	bio_put(bio);
	bio_endio(parent);
}

static void meta_end_read(struct bio *bio)
{
	struct bio *parent = bio->bi_private;

	if (!bio->bi_status)
		WRITE_ONCE(parent->bi_status, BLK_STS_OK);
	else
		pr_err("TODO: try other rdev\n");

	bio_put(bio);
	bio_endio(parent);
}

static void md_submit_meta_bio(struct bio *bio)
{
	struct bio *new;
	struct md_rdev *rdev;
	struct mddev *mddev = bio->bi_bdev->bd_disk->private_data;

	if (unlikely(bio->bi_opf & REQ_PREFLUSH))
		bio->bi_opf &= ~REQ_PREFLUSH;

	if (!bio_sectors(bio)) {
		bio_endio(bio);
		return;
	}

	/* status will be cleared if any member disk IO succeed */
	bio->bi_status = BLK_STS_IOERR;

	rdev_for_each(rdev, mddev) {
		if (rdev->raid_disk < 0 || test_bit(Faulty, &rdev->flags))
			continue;

		new = bio_alloc_clone(rdev->bdev, bio, GFP_NOIO, &mddev->sync_set);
		new->bi_iter.bi_sector = bio->bi_iter.bi_sector +
					 mddev->bitmap_info.offset +
					 rdev->sb_start;
		new->bi_opf |= REQ_SYNC | REQ_IDLE | REQ_META;
		bio_inc_remaining(bio);
		new->bi_private = bio;

		if (bio_data_dir(bio) == WRITE) {
			new->bi_end_io = meta_end_write;
			new->bi_opf |= REQ_FUA;
			submit_bio_noacct(new);
		} else {
			new->bi_end_io = meta_end_read;
			submit_bio_noacct(new);
			break;
		}
	}

	bio_endio(bio);
}

const struct block_device_operations md_meta_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= md_submit_meta_bio,
};

int md_alloc_meta_file(struct mddev *mddev)
{
	int ret;
	struct file *bdev_file;
	struct gendisk *disk = blk_alloc_disk(&mddev->gendisk->queue->limits,
					      NUMA_NO_NODE);

	if (!disk)
		return -ENOMEM;

	sprintf(disk->disk_name, "%s_meta", mdname(mddev));
	disk->flags |= GENHD_FL_HIDDEN;
	disk->fops = &md_meta_fops;

	ret = add_disk(disk);
	if (ret) {
		put_disk(disk);
		return ret;
	}

	/*
	 * Currently is only used for bitmap IO, so disk size is bitmap size, at
	 * most 64KB + super block.
	 */
	set_capacity(disk, 64 * 2 + (PAGE_SIZE >> SECTOR_SHIFT));
	bdev_file = bdev_file_alloc(disk->part0, BLK_OPEN_READ | BLK_OPEN_WRITE);
	if (IS_ERR(bdev_file)) {
		del_gendisk(disk);
		put_disk(disk);
		return PTR_ERR(bdev_file);
	}

	disk->private_data = mddev;
	mddev->meta_file = bdev_file;

	/* corresponding to the blkdev_put_no_open() from blkdev_release() */
	get_device(disk_to_dev(disk));

	bdev_file->f_flags |= O_LARGEFILE;
	bdev_file->f_mode |= FMODE_CAN_ODIRECT;
	bdev_file->f_mapping = disk->part0->bd_mapping;
	bdev_file->f_wb_err = filemap_sample_wb_err(bdev_file->f_mapping);

	/* not actually opened */
	bdev_file->private_data = ERR_PTR(-ENODEV);

	return 0;
}

void md_free_meta_file(struct mddev *mddev)
{
	struct gendisk *disk;
	struct file *bdev_file = mddev->meta_file;

	if (!bdev_file)
		return;

	mddev->meta_file = NULL;
	disk = file_bdev(bdev_file)->bd_disk;

	fput(bdev_file);
	del_gendisk(disk);
	put_disk(disk);
}
