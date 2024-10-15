// SPDX-License-Identifier: GPL-2.0-or-later

#include "md.h"
#include "md-bitmap.h"

static char bits[3] = {0, 1, 2};

#define LLBITMAP_MAJOR_HI 6

#define BIT_CLEAN bits[0]
#define BIT_DIRTY bits[1]
#define BIT_RESYNC bits[2]

struct llbitmap {
	struct mddev *mddev;

	/* chunksize by sector */
	unsigned long chunksize;
	/* chunksize in shift */
	unsigned long chunkshift;
	/* total number of bits */
	unsigned long chunks;

	atomic_t behind_writes;
	wait_queue_head_t behind_wait;

	unsigned long daemon_lastrun;
	struct work_struct daemon_work;

	/* bitmap IO from all underlying disks failed */
	bool io_error;
};

static bool llbitmap_enabled(struct mddev *mddev)
{
	return mddev->bitmap != NULL;
}

static void daemon_work_fn(struct work_struct *work)
{
	struct llbitmap *llbitmap = container_of(work, struct llbitmap, daemon_work);
	struct mddev *mddev = llbitmap->mddev;
	struct file *meta_file = mddev->meta_file;
	loff_t pos = PAGE_SIZE;
	char c;

	if (!meta_file || !llbitmap || llbitmap->io_error)
		return;

	/* wait for writes to be done, and clear all bits */
	mddev_suspend(mddev, false);

	while (pos < llbitmap->chunks + PAGE_SIZE) {
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			goto out;
		}

		if (c == BIT_DIRTY) {
			ret = kernel_write(meta_file, &BIT_CLEAN, 1, &pos);
			if (ret != 1) {
				llbitmap->io_error = true;
				goto out;
			}
		}

		pos++;
	}

	filemap_write_and_wait_range(meta_file->f_mapping, PAGE_SIZE, LLONG_MAX);

out:
	mddev_resume(mddev);
}

static int llbitmap_resize(struct mddev *mddev, sector_t blocks, int chunksize,
			   bool init)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	unsigned long chunks;

	if (!llbitmap)
		return 0;

	if (mddev->bitmap_info.file) {
		pr_err("md: doesn't support file-based llbitmap\n");
		return -EINVAL;
	}

	if (mddev->bitmap_info.external) {
		pr_err("md: doesn't support external llbitmap\n");
		return -EINVAL;
	}

	if (mddev->bitmap_info.space == 0) {
		pr_err("md: no space for llbitmap\n");
		return -EINVAL;
	}

	if (chunksize != 0) {
		pr_err("md: todo: set chunksize for llbitmap\n");
		return -EINVAL;
	}

	/* make sure all bits are clean before resize */
	if (!init) {
		flush_work(&llbitmap->daemon_work);
		queue_work(md_bitmap_wq, &llbitmap->daemon_work);
		flush_work(&llbitmap->daemon_work);
	}

	blocks = roundup_pow_of_two(blocks);
	chunks = rounddown_pow_of_two(mddev->bitmap_info.space << SECTOR_SHIFT);

	llbitmap->chunks = chunks;
	do_div(blocks, chunks);
	llbitmap->chunksize = blocks;
	if (llbitmap->chunksize == 0) {
		pr_err("md: blocks less than chunks\n");
		return -EINVAL;
	}

	llbitmap->chunkshift = ilog2(llbitmap->chunksize);
	mddev->bitmap_info.chunksize = llbitmap->chunksize;
	mddev->bitmap_info.daemon_sleep = 30 * HZ;
	mddev->bitmap_info.max_write_behind = COUNTER_MAX / 2;

	return 0;
}

static void llbitmap_dirty_bits(struct mddev *mddev, unsigned long s,
				unsigned long e)
{
	struct file *meta_file = mddev->meta_file;
	struct llbitmap *llbitmap = mddev->bitmap;
	loff_t pos = s + PAGE_SIZE;
	char c;

	if (!meta_file || !llbitmap || llbitmap->io_error)
		return;

	while (pos <= e + PAGE_SIZE) {
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			return;
		}

		if (c != BIT_DIRTY && c != BIT_RESYNC) {
			ret = kernel_write(meta_file, &BIT_DIRTY, 1, &pos);
			if (ret != 1) {
				llbitmap->io_error = true;
				break;
			}
		}

		pos++;
	}
}

static int llbitmap_new_sb(struct llbitmap *llbitmap)
{
	struct mddev *mddev = llbitmap->mddev;
	struct file *meta_file = mddev->meta_file;
	struct page *sb_page;
	bitmap_super_t *sb;
	int ret;

	mddev->bitmap_info.space = super_1_choose_bm_space(mddev->resync_max_sectors);
	ret = llbitmap_resize(mddev, mddev->resync_max_sectors, 0, true);
	if (ret)
		return ret;

	sb_page = pagecache_get_page(meta_file->f_mapping, 0,
				     FGP_LOCK | FGP_CREAT | FGP_WRITE,
				     GFP_KERNEL);
	sb = kmap_local_page(sb_page);

	memset(sb, 0, PAGE_SIZE);

	sb->magic = cpu_to_le32(LLBITMAP_MAGIC);
	sb->version = cpu_to_le32(LLBITMAP_MAJOR_HI);
	sb->chunksize = cpu_to_le32(llbitmap->chunksize);

	sb->daemon_sleep = cpu_to_le32(mddev->bitmap_info.daemon_sleep);
	sb->write_behind = cpu_to_le32(mddev->bitmap_info.max_write_behind);
	sb->sync_size = cpu_to_le64(mddev->resync_max_sectors);
	sb->events_cleared = cpu_to_le64(mddev->events);

	memcpy(sb->uuid, mddev->uuid, 16);

	kunmap_local(sb);
	set_page_dirty(sb_page);
	put_page(sb_page);
	unlock_page(sb_page);

	/* set all bits for new bitmap. */
	llbitmap_dirty_bits(mddev, 0, llbitmap->chunks);
	return filemap_write_and_wait_range(meta_file->f_mapping, 0, LLONG_MAX);
}

static int llbitmap_read_sb(struct llbitmap *llbitmap)
{
	struct mddev *mddev = llbitmap->mddev;
	struct page *sb_page;
	bitmap_super_t *sb;

	sb_page = read_mapping_page(mddev->meta_file->f_mapping, 0, NULL);
	sb = kmap_local_page(sb_page);

	if (sb->magic != cpu_to_le32(LLBITMAP_MAGIC)) {
		pr_err("%s: %s: bad magic %x\n", __func__, mdname(mddev), le32_to_cpu(sb->magic));
		goto err_out;
	}

	if (sb->version != cpu_to_le32(LLBITMAP_MAJOR_HI)) {
		pr_err("%s: %s: bad version\n", __func__, mdname(mddev));
		goto err_out;
	}

	if (sb->sync_size != cpu_to_le64(mddev->resync_max_sectors)) {
		pr_err("%s: %s: bad size\n", __func__, mdname(mddev));
		goto err_out;
	}

	kunmap_local(sb);
	put_page(sb_page);
	mddev->bitmap_info.space = super_1_choose_bm_space(mddev->resync_max_sectors);
	return llbitmap_resize(mddev, mddev->resync_max_sectors, 0, true);

err_out:
	kunmap_local(sb);
	put_page(sb_page);
	return -EINVAL;
}

static int llbitmap_init_sb(struct llbitmap *llbitmap)
{
	int ret;

	ret = llbitmap_read_sb(llbitmap);
	if (ret != 0)
		ret = llbitmap_new_sb(llbitmap);

	return ret;
}

static int llbitmap_create(struct mddev *mddev, int slot)
{
	int ret;
	struct llbitmap *llbitmap;

	/* don't support md-cluster yet */
	if (slot != -1)
		return -EOPNOTSUPP;

	llbitmap = kzalloc(sizeof(*llbitmap), GFP_KERNEL);
	if (!llbitmap)
		return -ENOMEM;

	atomic_set(&llbitmap->behind_writes, 0);
	init_waitqueue_head(&llbitmap->behind_wait);
	INIT_WORK(&llbitmap->daemon_work, daemon_work_fn);
	llbitmap->mddev = mddev;
	mddev->bitmap = llbitmap;

	ret = llbitmap_init_sb(llbitmap);
	if (ret) {
		mddev->bitmap = NULL;
		kfree(llbitmap);
		return ret;
	}

	mddev_set_timeout(mddev, 30, false);

	return 0;
}

static int llbitmap_load(struct mddev *mddev)
{
	struct file *meta_file = mddev->meta_file;
	struct llbitmap *llbitmap = mddev->bitmap;
	loff_t pos = PAGE_SIZE;
	char c;

	if (!meta_file || !llbitmap || llbitmap->io_error)
		return 0;

	while (pos < llbitmap->chunks + PAGE_SIZE) {
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			return -EIO;
		}

		/* mark dirty bits as need resync. */
		if (c == BIT_DIRTY) {
			ret = kernel_write(meta_file, &BIT_RESYNC, 1, &pos);
			if (ret != 1) {
				llbitmap->io_error = true;
				return -EIO;
			}
		}

		pos++;
	}

	return 0;
}

static void llbitmap_wait_behind_writes(struct mddev *mddev)
{
	struct llbitmap *llbitmap = mddev->bitmap;

	if (!llbitmap || !atomic_read(&llbitmap->behind_writes))
		return;

	wait_event(llbitmap->behind_wait, !atomic_read(&llbitmap->behind_writes));
}

static void llbitmap_destroy(struct mddev *mddev)
{
	struct llbitmap *llbitmap = mddev->bitmap;

	if (!llbitmap)
		return;

	llbitmap_wait_behind_writes(mddev);

	mutex_lock(&mddev->bitmap_info.mutex);

	/* no new daemon work */
	mddev->bitmap = NULL;
	mddev_set_timeout(mddev, MAX_SCHEDULE_TIMEOUT, true);

	/* wait for pending work to be done */
	flush_work(&llbitmap->daemon_work);

	mutex_unlock(&mddev->bitmap_info.mutex);

	kfree(llbitmap);
}

static void llbitmap_flush(struct mddev *mddev)
{
	struct file *meta_file = mddev->meta_file;

	if (!meta_file)
		return;

	filemap_write_and_wait_range(meta_file->f_mapping, 0, LLONG_MAX);
}

/*
 * This is used to mark pages in memory as needing writeback for md-bitmap.c,
 * nothing to do here
 */
static void llbitmap_write_all(struct mddev *mddev)
{

}

static void llbitmap_unplug(struct mddev *mddev, bool sync)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	struct file *meta_file = mddev->meta_file;

	if (!llbitmap || llbitmap->io_error || !meta_file)
		return;

	/* TODO: plug level: file_write_and_wait_range() */
	filemap_write_and_wait_range(meta_file->f_mapping, PAGE_SIZE, LLONG_MAX);
}

static void llbitmap_daemon_work(struct mddev *mddev)
{
	struct llbitmap *llbitmap = mddev->bitmap;

	if (!llbitmap || llbitmap->io_error || !mddev->meta_file)
		return;

	mutex_lock(&mddev->bitmap_info.mutex);

	if (time_before(jiffies, llbitmap->daemon_lastrun +
				 mddev->bitmap_info.daemon_sleep))
		goto done;

	if (work_busy(&llbitmap->daemon_work))
		goto done;

	llbitmap->daemon_lastrun = jiffies;
	queue_work(md_bitmap_wq, &llbitmap->daemon_work);

done:
	mutex_unlock(&mddev->bitmap_info.mutex);
}

static int llbitmap_startwrite(struct mddev *mddev, sector_t offset,
			       unsigned long sectors, bool behind)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	unsigned long start;
	unsigned long end;

	if (!llbitmap || mddev->meta_file)
		return 0;

	if (behind)
		atomic_inc(&llbitmap->behind_writes);

	start = offset >> llbitmap->chunkshift;
	end = (offset + sectors) >> llbitmap->chunkshift;
	llbitmap_dirty_bits(mddev, start, end);

	return 0;
}

static void llbitmap_endwrite(struct mddev *mddev, sector_t offset,
			      unsigned long sectors, bool success, bool behind)
{
	struct llbitmap *llbitmap = mddev->bitmap;

	if (behind && llbitmap && atomic_dec_and_test(&llbitmap->behind_writes))
		wake_up(&llbitmap->behind_wait);
}

static bool llbitmap_start_sync(struct mddev *mddev, sector_t offset,
				sector_t *blocks, bool degraded)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	struct file *meta_file = mddev->meta_file;
	bool rv = false;
	loff_t pos;
	*blocks = 0;

	/* sync all blocks */
	if (!llbitmap || !meta_file || llbitmap->io_error) {
		*blocks = 1024;
		return true;
	}

	pos = (offset >> llbitmap->chunkshift) + PAGE_SIZE;
	while (*blocks < PAGE_SIZE >> SECTOR_SHIFT &&
	       pos < llbitmap->chunks + PAGE_SIZE) {
		char c;
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			*blocks = 1024;
			return true;
		}

		if (c == BIT_RESYNC)
			rv = true;

		*blocks += llbitmap->chunksize;
		pos++;
	}

	return rv;
}

static void llbitmap_end_sync(struct mddev *mddev, sector_t offset,
			      sector_t *blocks)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	struct file *meta_file = mddev->meta_file;
	loff_t pos = PAGE_SIZE;
	loff_t end;

	/* sync all blocks */
	if (!llbitmap || !meta_file || llbitmap->io_error) {
		*blocks = 1024;
		return;
	}

	end = (offset >> llbitmap->chunkshift) + PAGE_SIZE;
	while (pos <= end) {
		char c;
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			*blocks = 1024;
			return;
		}

		if (c == BIT_RESYNC) {
			/* daemon is responsible to clear the bit. */
			ret = kernel_write(meta_file, &BIT_DIRTY, 1, &pos);

			if (ret != 1) {
				llbitmap->io_error = true;
				*blocks = 1024;
				return;
			}
		}

		*blocks += llbitmap->chunksize;
		pos++;
	}
}

static void llbitmap_close_sync(struct mddev *mddev)
{
	struct llbitmap *llbitmap = mddev->bitmap;
	struct file *meta_file = mddev->meta_file;
	loff_t pos = PAGE_SIZE;

	if (!llbitmap || !meta_file || llbitmap->io_error)
		return;

	while (pos < llbitmap->chunks + PAGE_SIZE) {
		char c;
		ssize_t ret = kernel_read(meta_file, &c, 1, &pos);

		if (ret != 1) {
			llbitmap->io_error = true;
			return;
		}

		if (c == BIT_RESYNC) {
			ret = kernel_write(meta_file, &BIT_DIRTY, 1, &pos);

			if (ret != 1) {
				llbitmap->io_error = true;
				return;
			}
		}

		pos++;
	}
}

static void llbitmap_cond_end_sync(struct mddev *mddev, sector_t sector,
				   bool force)
{
	sector_t blocks;
	struct llbitmap *llbitmap = mddev->bitmap;
	struct file *meta_file = mddev->meta_file;

	if (!llbitmap || !meta_file || llbitmap->io_error)
		return;

	if (sector == 0 || !force)
		return;

	wait_event(mddev->recovery_wait,
		   atomic_read(&mddev->recovery_active) == 0);

	llbitmap_end_sync(mddev, sector, &blocks);
	mddev->curr_resync_completed = sector;
	set_bit(MD_SB_CHANGE_CLEAN, &mddev->sb_flags);
	sysfs_notify_dirent_safe(mddev->sysfs_completed);
}

static void llbitmap_update_sb(void *data)
{
	bitmap_super_t *sb;
	struct mddev *mddev;
	struct page *sb_page;
	struct llbitmap *llbitmap = data;

	if (!llbitmap || llbitmap->io_error)
		return;

	mddev = llbitmap->mddev;
	if (!mddev || !mddev->meta_file)
		return;

retry:
	sb_page = read_mapping_page(mddev->meta_file->f_mapping, 0, NULL);
	if (IS_ERR(sb_page)) {
		/* other better solution? */
		if (PTR_ERR(sb_page) == -ENOMEM)
			goto retry;

		llbitmap->io_error = true;
		return;
	}

	sb = kmap_local_page(sb_page);

	sb->chunksize = cpu_to_le32(llbitmap->chunksize);
	sb->daemon_sleep = cpu_to_le32(mddev->bitmap_info.daemon_sleep);
	sb->write_behind = cpu_to_le32(mddev->bitmap_info.max_write_behind);
	sb->sync_size = cpu_to_le64(mddev->resync_max_sectors);
	sb->events_cleared = cpu_to_le64(mddev->events);

	kunmap_local(sb);
	set_page_dirty(sb_page);
	put_page(sb_page);

	if (filemap_write_and_wait_range(mddev->meta_file->f_mapping,
					 0, PAGE_SIZE) != 0)
		llbitmap->io_error = true;
}

static int llbitmap_get_stats(void *data, struct md_bitmap_stats *stats)
{
	struct llbitmap *llbitmap = data;

	if (!llbitmap)
		return -ENOENT;

	memset(stats, 0, sizeof(*stats));
	stats->behind_writes = atomic_read(&llbitmap->behind_writes);
	return 0;
}

static void llbitmap_sync_with_cluster(struct mddev *mddev,
				       sector_t old_lo, sector_t old_hi,
				       sector_t new_lo, sector_t new_hi)
{
}

static void *llbitmap_get_from_slot(struct mddev *mddev, int slot)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static int llbitmap_copy_from_slot(struct mddev *mddev, int slot, sector_t *low,
				   sector_t *high, bool clear_bits)
{
	return -EOPNOTSUPP;
}

static void llbitmap_set_pages(void *data, unsigned long pages)
{
}

static void md_llbitmap_free(void *data)
{
}

static ssize_t
timeout_show(struct mddev *mddev, char *page)
{
	ssize_t len;
	unsigned long secs = mddev->bitmap_info.daemon_sleep / HZ;
	unsigned long jifs = mddev->bitmap_info.daemon_sleep % HZ;

	len = sprintf(page, "%lu", secs);
	if (jifs)
		len += sprintf(page+len, ".%03u", jiffies_to_msecs(jifs));
	len += sprintf(page+len, "\n");
	return len;
}

static ssize_t
timeout_store(struct mddev *mddev, const char *buf, size_t len)
{
	/* timeout can be set at any time */
	unsigned long timeout;
	int rv = strict_strtoul_scaled(buf, &timeout, 4);

	if (rv)
		return rv;

	/* just to make sure we don't overflow... */
	if (timeout >= LONG_MAX / HZ)
		return -EINVAL;

	timeout = timeout * HZ / 10000;

	if (timeout >= MAX_SCHEDULE_TIMEOUT)
		timeout = MAX_SCHEDULE_TIMEOUT-1;
	if (timeout < 1)
		timeout = 1;

	mddev->bitmap_info.daemon_sleep = timeout;
	mddev_set_timeout(mddev, timeout, false);
	md_wakeup_thread(mddev->thread);

	return len;
}

static struct md_sysfs_entry llbitmap_timeout =
__ATTR(time_base, 0644, timeout_show, timeout_store);

static struct attribute *md_llbitmap_attrs[] = {
	&llbitmap_timeout.attr,
	NULL
};

static struct attribute_group md_llbitmap_group = {
	.name = "llbitmap",
	.attrs = md_llbitmap_attrs,
};

static struct bitmap_operations llbitmap_ops = {
	.version		= 2,

	.enabled		= llbitmap_enabled,
	.create			= llbitmap_create,
	.resize			= llbitmap_resize,
	.load			= llbitmap_load,
	.destroy		= llbitmap_destroy,
	.flush			= llbitmap_flush,
	.write_all		= llbitmap_write_all,
	.dirty_bits		= llbitmap_dirty_bits,
	.unplug			= llbitmap_unplug,
	.daemon_work		= llbitmap_daemon_work,
	.wait_behind_writes	= llbitmap_wait_behind_writes,

	.startwrite		= llbitmap_startwrite,
	.endwrite		= llbitmap_endwrite,
	.start_sync		= llbitmap_start_sync,
	.end_sync		= llbitmap_end_sync,
	.cond_end_sync		= llbitmap_cond_end_sync,
	.close_sync		= llbitmap_close_sync,

	.update_sb		= llbitmap_update_sb,
	.get_stats		= llbitmap_get_stats,

	.sync_with_cluster	= llbitmap_sync_with_cluster,
	.get_from_slot		= llbitmap_get_from_slot,
	.copy_from_slot		= llbitmap_copy_from_slot,
	.set_pages		= llbitmap_set_pages,
	.free			= md_llbitmap_free,

	.group			= &md_llbitmap_group,
};

void mddev_set_llbitmap_ops(struct mddev *mddev)
{
	mddev->bitmap_ops = &llbitmap_ops;
}
