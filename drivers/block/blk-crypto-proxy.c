// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "blk-crypto-proxy: " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rcupdate.h>
#include <linux/uio.h>
#include <linux/blk-crypto.h>
#include <linux/blk-crypto-profile.h>
#include <linux/blk-crypto-proxy.h>
#include <linux/virtio_blk.h>

static const struct bcp_hypervisor_ops __rcu *g_hypervisor_ops;
static DEFINE_MUTEX(g_hypervisor_ops_lock);

static const struct bcp_slot_virt_ops __rcu *g_slot_virt_ops;
static DEFINE_MUTEX(g_slot_virt_ops_lock);

int bcp_register_hypervisor_ops(const struct bcp_hypervisor_ops *ops)
{
	int ret = 0;

	mutex_lock(&g_hypervisor_ops_lock);
	if (rcu_access_pointer(g_hypervisor_ops))
		ret = -EBUSY;
	else
		rcu_assign_pointer(g_hypervisor_ops, ops);
	mutex_unlock(&g_hypervisor_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(bcp_register_hypervisor_ops);

void bcp_unregister_hypervisor_ops(const struct bcp_hypervisor_ops *ops)
{
	mutex_lock(&g_hypervisor_ops_lock);
	if (rcu_access_pointer(g_hypervisor_ops) == ops)
		rcu_assign_pointer(g_hypervisor_ops, NULL);
	mutex_unlock(&g_hypervisor_ops_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(bcp_unregister_hypervisor_ops);

int bcp_register_slot_virt_ops(const struct bcp_slot_virt_ops *ops)
{
	int ret = 0;

	mutex_lock(&g_slot_virt_ops_lock);
	if (rcu_access_pointer(g_slot_virt_ops))
		ret = -EBUSY;
	else
		rcu_assign_pointer(g_slot_virt_ops, ops);
	mutex_unlock(&g_slot_virt_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(bcp_register_slot_virt_ops);

void bcp_unregister_slot_virt_ops(const struct bcp_slot_virt_ops *ops)
{
	mutex_lock(&g_slot_virt_ops_lock);
	if (rcu_access_pointer(g_slot_virt_ops) == ops)
		rcu_assign_pointer(g_slot_virt_ops, NULL);
	mutex_unlock(&g_slot_virt_ops_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(bcp_unregister_slot_virt_ops);

/**
 * struct bcp_ctx - per-fd state for /dev/blk-crypto-proxy
 * @bdev_file: file handle for the bound block device; NULL until BCP_BIND_CONTEXT.
 *             Published with smp_store_release() so hot-path ioctls can read it
 *             lock-free via smp_load_acquire() in bcp_ctx_bound().
 * @guest_id: guest identifier resolved from vm_fd at bind time.
 * @bdev_writable: block_dev_fd was opened with write access.
 * @bind_lock: serializes concurrent BCP_BIND_CONTEXT calls on this fd.
 */
struct bcp_ctx {
	struct file *bdev_file;
	u32 guest_id;
	bool bdev_writable;
	struct mutex bind_lock;
};

/*
 * True once BCP_BIND_CONTEXT has published ctx->bdev_file.  The acquire pairs
 * with smp_store_release() in bcp_ioctl_bind_context(), ensuring ctx->guest_id
 * and ctx->bdev_writable are visible to any caller that observes true.
 */
static bool bcp_ctx_bound(struct bcp_ctx *ctx)
{
	return smp_load_acquire(&ctx->bdev_file) != NULL;
}

static int bcp_open(struct inode *inode, struct file *file)
{
	struct bcp_ctx *ctx;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	mutex_init(&ctx->bind_lock);
	file->private_data = ctx;
	return 0;
}

static int bcp_release(struct inode *inode, struct file *file)
{
	struct bcp_ctx *ctx = file->private_data;

	if (ctx) {
		if (ctx->bdev_file)
			bdev_fput(ctx->bdev_file);
		mutex_destroy(&ctx->bind_lock);
		kfree(ctx);
		file->private_data = NULL;
	}
	return 0;
}

/*
 * Resolve a userspace block device fd to a struct file holding a reference
 * to the block device, opened with the same access mode as @fd so that a
 * read-only fd cannot gain write access via BCP_SUBMIT_IO_BY_VSLOT.
 */
static struct file *bcp_bdev_from_fd(int fd, bool *writable)
{
	struct file *f;
	struct inode *inode;
	dev_t dev;
	blk_mode_t mode = 0;

	f = fget(fd);
	if (!f)
		return ERR_PTR(-EBADF);
	inode = file_inode(f);
	if (!S_ISBLK(inode->i_mode)) {
		fput(f);
		return ERR_PTR(-ENOTBLK);
	}
	if (f->f_mode & FMODE_READ)
		mode |= BLK_OPEN_READ;
	if (f->f_mode & FMODE_WRITE)
		mode |= BLK_OPEN_WRITE;
	if (!mode) {
		fput(f);
		return ERR_PTR(-EACCES);
	}
	*writable = !!(mode & BLK_OPEN_WRITE);
	dev = inode->i_rdev;
	fput(f);
	return bdev_file_open_by_dev(dev, mode, NULL, NULL);
}

static long bcp_ioctl_bind_context(struct file *file,
				   struct bcp_bind_context_arg __user *argp)
{
	struct bcp_ctx *ctx = file->private_data;
	struct bcp_bind_context_arg arg;
	const struct bcp_hypervisor_ops *hv_ops;
	struct file *bdev_file;
	u32 guest_id;
	bool writable = false;
	int ret;

	if (!ctx)
		return -EINVAL;
	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	/*
	 * get_guest_id() may sleep; call it before taking bind_lock.
	 */
	rcu_read_lock();
	hv_ops = rcu_dereference(g_hypervisor_ops);
	if (!hv_ops) {
		rcu_read_unlock();
		return -EOPNOTSUPP;
	}
	ret = hv_ops->get_guest_id(arg.vm_fd, &guest_id);
	rcu_read_unlock();
	if (ret)
		return ret;

	/*
	 * Serialize against concurrent BCP_BIND_CONTEXT calls: two callers
	 * could both pass the ctx->bdev_file == NULL check before either stores.
	 */
	guard(mutex)(&ctx->bind_lock);

	if (ctx->bdev_file)
		return -EBUSY;

	bdev_file = bcp_bdev_from_fd(arg.block_dev_fd, &writable);
	if (IS_ERR(bdev_file))
		return PTR_ERR(bdev_file);

	ctx->guest_id = guest_id;
	ctx->bdev_writable = writable;
	/*
	 * Publish ctx->bdev_file last with a release barrier; bcp_ctx_bound()
	 * reads it with smp_load_acquire() without taking @bind_lock.
	 */
	smp_store_release(&ctx->bdev_file, bdev_file);
	return 0;
}

/*
 * Maps VIRTIO_BLK_CRYPTO_MODE_* to enum blk_crypto_mode_num.  The two index
 * spaces do not coincide, so modes_supported[] must not be copied positionally.
 * Keep in sync with virtio_blk_crypto_mode_map[] in drivers/block/virtio_blk.c.
 */
static const enum blk_crypto_mode_num
	bcp_virtio_crypto_mode_map[VIRTIO_BLK_CRYPTO_MODE_MAX + 1] = {
	[VIRTIO_BLK_CRYPTO_MODE_INVALID]	= BLK_ENCRYPTION_MODE_INVALID,
	[VIRTIO_BLK_CRYPTO_MODE_AES_256_XTS]	= BLK_ENCRYPTION_MODE_AES_256_XTS,
};

static long bcp_ioctl_get_crypto_caps(struct file *file,
				      struct bcp_get_crypto_caps_arg __user *argp)
{
	struct bcp_ctx *ctx = file->private_data;
	struct bcp_get_crypto_caps_arg arg;
	struct block_device *bdev;
	u32 modes[VIRTIO_BLK_CRYPTO_MODE_MAX + 1] = {0};
	struct blk_crypto_profile *profile;
	unsigned int i, n;
	u32 cap, written;

	if (!ctx || !bcp_ctx_bound(ctx))
		return -ENXIO;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;
	if (arg.num_modes && !arg.modes_ptr)
		return -EINVAL;

	bdev = file_bdev(ctx->bdev_file);
	profile = bdev_get_queue(bdev)->crypto_profile;
	if (!profile)
		return -EOPNOTSUPP;

	arg.key_types_supported = profile->key_types_supported;
	/*
	 * Clamp to 8: the @dun wire field is a single __aligned_u64 so nothing
	 * upstream can deliver a wider DUN regardless of what the profile claims.
	 */
	arg.max_dun_bytes = min_t(u32, profile->max_dun_bytes_supported, 8);

	/*
	 * arg.modes_supported[] is indexed by VIRTIO_BLK_CRYPTO_MODE_* (index 0
	 * is always 0 per the virtio spec).  Translate via the map above; do not
	 * copy profile->modes_supported[] positionally.
	 */
	n = VIRTIO_BLK_CRYPTO_MODE_MAX + 1;
	cap = arg.num_modes;
	written = min_t(u32, cap, n);

	for (i = 1; i < n; i++) {
		enum blk_crypto_mode_num kmode = bcp_virtio_crypto_mode_map[i];

		if (!kmode)
			continue;
		modes[i] = profile->modes_supported[kmode];
	}

	if (written &&
	    copy_to_user(u64_to_user_ptr(arg.modes_ptr), modes,
			 written * sizeof(modes[0])))
		return -EFAULT;
	arg.num_modes = written;

	arg.max_slots = 0;
	rcu_read_lock();
	{
		const struct bcp_slot_virt_ops *sv_ops =
				rcu_dereference(g_slot_virt_ops);
		if (sv_ops) {
			int nslots = sv_ops->get_guest_slots(profile, ctx->guest_id);

			if (nslots > 0)
				arg.max_slots = nslots;
		}
	}
	rcu_read_unlock();

	if (copy_to_user(argp, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

/*
 * Compute the number of pages needed for up to @want_bytes of iovec data
 * starting at cursor (@start_idx, @start_off).  The page count is capped at
 * @cap to bound the arithmetic; *bytes_out receives the actual byte count.
 */
static unsigned int bcp_iov_pages_for_bytes(const struct iovec *iov, u32 iov_cnt,
					    u32 start_idx, u64 start_off,
					      u64 want_bytes, unsigned int cap,
					      u64 *bytes_out)
{
	u64 pages = 0, taken = 0;
	u32 i;

	for (i = start_idx; i < iov_cnt && taken < want_bytes; i++) {
		u64 base, len;

		if (i == start_idx) {
			base = (u64)(uintptr_t)iov[i].iov_base + start_off;
			len  = iov[i].iov_len - start_off;
		} else {
			base = (u64)(uintptr_t)iov[i].iov_base;
			len  = iov[i].iov_len;
		}
		if (len == 0)
			continue;
		if (len > want_bytes - taken)
			len = want_bytes - taken;
		pages += DIV_ROUND_UP(len + offset_in_page(base), PAGE_SIZE);
		taken += len;
		if (pages >= cap) {
			*bytes_out = taken;
			return cap;
		}
	}
	*bytes_out = taken;
	return (unsigned int)pages;
}

/*
 * Advance cursor (*idx, *off) forward by @bytes within @iov[0..iov_cnt).
 */
static void bcp_iov_advance_cursor(const struct iovec *iov, u32 iov_cnt,
				   u32 *idx, u64 *off, u64 bytes)
{
	while (bytes > 0 && *idx < iov_cnt) {
		u64 seg_remaining = iov[*idx].iov_len - *off;
		u64 take = min_t(u64, seg_remaining, bytes);

		*off += take;
		bytes -= take;
		if (*off == iov[*idx].iov_len) {
			(*idx)++;
			*off = 0;
		}
	}
}

static long bcp_ioctl_submit_io_by_vslot(struct file *file,
					 struct bcp_submit_io_by_vslot_arg __user *argp)
{
	struct bcp_ctx *ctx = file->private_data;
	struct bcp_submit_io_by_vslot_arg arg;
	struct block_device *bdev;
	struct blk_crypto_profile *profile;
	struct iovec *iov = NULL;
	struct iov_iter iter;
	struct blk_crypto_slot slot;
	u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
	u64 bytes_done = 0;
	u64 align, stride;
	u64 total_bytes;
	u32 seg_idx = 0;
	u64 seg_off = 0;
	unsigned int phy_slot;
	int ret = -EFAULT;

	if (!ctx || !bcp_ctx_bound(ctx))
		return -ENXIO;

	if (copy_from_user(&arg, argp, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved2)
		return -EINVAL;

	bdev = file_bdev(ctx->bdev_file);

	if (arg.direction != BCP_DIR_READ && arg.direction != BCP_DIR_WRITE)
		return -EINVAL;
	/*
	 * blk_mode_t does not stop submit_bio() from writing; enforce the
	 * caller's original fd permission explicitly.
	 */
	if (arg.direction == BCP_DIR_WRITE && !ctx->bdev_writable)
		return -EACCES;
	/*
	 * bdev_read_only() can change after bind time; submit_bio_noacct()'s
	 * bio_check_ro() only warns rather than errors in this kernel.
	 */
	if (arg.direction == BCP_DIR_WRITE && bdev_read_only(bdev))
		return -EROFS;
	if (arg.flags != BCP_SUBMIT_IO_F_IOV)
		return -EINVAL;
	if (arg.iov_cnt == 0 || arg.iov_cnt > BCP_MAX_IOV)
		return -EINVAL;
	/* A shift amount >= 64 would be undefined behavior. */
	if (arg.data_unit_size_bits >= 64)
		return -EINVAL;

	profile = bdev_get_queue(bdev)->crypto_profile;
	if (!profile)
		return -EOPNOTSUPP;

	/* Resolve virt_slot → phy_slot. */
	rcu_read_lock();
	{
		const struct bcp_slot_virt_ops *sv_ops =
				rcu_dereference(g_slot_virt_ops);
		if (!sv_ops) {
			rcu_read_unlock();
			return -EOPNOTSUPP;
		}
		ret = sv_ops->vslot_to_pslot(profile, ctx->guest_id,
					     arg.virt_slot, &phy_slot);
	}
	rcu_read_unlock();
	if (ret)
		return ret;

	memset(dun, 0, sizeof(dun));
	dun[0] = arg.dun;

	slot.phy_slot            = phy_slot;
	slot.data_unit_size_bits = arg.data_unit_size_bits;

	align = 1ULL << arg.data_unit_size_bits;
	/*
	 * Split bios at stride (smallest multiple of the data unit size >=
	 * PAGE_SIZE) boundaries so each bio ends on a whole data unit.
	 * bio_crypt_check_alignment() is skipped for slot-based bios (bc_key
	 * == NULL), so a mid-unit split would silently mis-encrypt/mis-decrypt.
	 */
	stride = DIV_ROUND_UP(PAGE_SIZE, align) * align;

	/*
	 * Import the caller's iovec once.  import_iovec() validates every
	 * segment with access_ok(), returns the total byte count, and takes a
	 * private kernel copy that eliminates TOCTOU from a guest mutating its
	 * own iovec array mid-ioctl.
	 */
	ret = import_iovec(arg.direction == BCP_DIR_READ ? ITER_DEST : ITER_SOURCE,
			   (const struct iovec __user *)u64_to_user_ptr(arg.iov_ptr),
			   arg.iov_cnt, 0, &iov, &iter);
	if (ret < 0)
		return ret;
	total_bytes = ret;

	/*
	 * Reject a misaligned total length up front: bio_crypt_check_alignment()
	 * is skipped for slot-based bios so nothing downstream will catch it.
	 */
	if (total_bytes == 0 || (total_bytes & (align - 1)) ||
	    (total_bytes & (SECTOR_SIZE - 1))) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Fail fast if the request exceeds the device.  bio_check_eod() would
	 * also catch this, but only on the last bio after earlier bios have
	 * already done real I/O.
	 */
	{
		sector_t nr_sectors = total_bytes >> SECTOR_SHIFT;
		sector_t maxsector = bdev_nr_sectors(bdev);

		if (nr_sectors > maxsector || arg.sector > maxsector - nr_sectors) {
			ret = -EIO;
			goto out;
		}
	}

	/*
	 * Reject an out-of-range DUN: slot-based bios skip
	 * bio_crypt_check_alignment(), so an overflow would silently truncate
	 * in the hardware DUN field rather than error out.
	 */
	{
		u64 total_units = total_bytes >> arg.data_unit_size_bits;
		u64 max_dun_used, dun_limit;

		if (check_add_overflow(arg.dun, total_units - 1, &max_dun_used)) {
			ret = -EINVAL;
			goto out;
		}
		dun_limit = profile->max_dun_bytes_supported >= 8 ? U64_MAX :
			(1ULL << (8 * profile->max_dun_bytes_supported)) - 1;
		if (max_dun_used > dun_limit) {
			ret = -EINVAL;
			goto out;
		}
	}

	/*
	 * Submit the request as a sequence of bios (submit_bio_wait() per
	 * bio), each holding at most BIO_MAX_VECS pages.  Sequential
	 * submission avoids DUN/IV correctness concerns across concurrent
	 * in-flight bios.
	 */
	while (seg_idx < arg.iov_cnt) {
		unsigned int pages_used = 0;
		u64 bio_bytes = 0;
		u32 la_idx = seg_idx;
		u64 la_off = seg_off;
		u64 remaining_before;
		struct bio *bio;

		/*
		 * Lookahead: count how many whole stride units fit within a
		 * fresh bio's BIO_MAX_VECS page budget.
		 */
		for (;;) {
			u64 unit_bytes;
			unsigned int unit_pages;

			unit_pages = bcp_iov_pages_for_bytes(iov, arg.iov_cnt,
							     la_idx, la_off, stride,
							BIO_MAX_VECS + 1,
							&unit_bytes);
			if (unit_bytes == 0)
				break; /* only empty segments remain */

			if (pages_used + unit_pages > BIO_MAX_VECS) {
				if (pages_used == 0) {
					/* data_unit_size_bits too large to fit one unit. */
					ret = -EINVAL;
					goto out;
				}
				break; /* finalize this bio; unit deferred to next */
			}

			pages_used += unit_pages;
			bio_bytes  += unit_bytes;
			bcp_iov_advance_cursor(iov, arg.iov_cnt, &la_idx, &la_off,
					       unit_bytes);
		}

		if (bio_bytes == 0)
			break;

		bio = bio_alloc(bdev, pages_used,
				arg.direction == BCP_DIR_WRITE ?
					REQ_OP_WRITE : REQ_OP_READ,
				GFP_KERNEL);
		if (!bio) {
			ret = -ENOMEM;
			goto out;
		}
		bio->bi_iter.bi_sector = arg.sector + (bytes_done >> SECTOR_SHIFT);

		/*
		 * Use bio_iov_iter_get_pages() to pin pages into the bio,
		 * the same as the O_DIRECT path.  Truncate the iter to this
		 * bio's byte budget, then reexpand for the next iteration.
		 */
		remaining_before = iov_iter_count(&iter);
		iov_iter_truncate(&iter, bio_bytes);
		ret = bio_iov_iter_get_pages(bio, &iter, 0, 0);
		if (ret < 0) {
			bio_put(bio);
			goto out;
		}
		if (iov_iter_count(&iter) != 0) {
			/*
			 * The lookahead verified bio_bytes fits in BIO_MAX_VECS;
			 * if bio_iov_iter_get_pages() stopped early, its page
			 * accounting disagreed with bcp_iov_pages_for_bytes().
			 */
			bio_put(bio);
			ret = -EIO;
			goto out;
		}
		iov_iter_reexpand(&iter, remaining_before - bio_bytes);

		/*
		 * Match __blkdev_direct_IO(): mark pages dirty on reads into
		 * user-backed memory.
		 */
		if (arg.direction == BCP_DIR_READ && user_backed_iter(&iter))
			bio_set_pages_dirty(bio);

		bcp_iov_advance_cursor(iov, arg.iov_cnt, &seg_idx, &seg_off,
				       bio_bytes);

		bio_crypt_set_ctx_by_slot(bio, &slot, dun, GFP_KERNEL);

		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			goto out;

		/*
		 * Advance dun by this bio's contribution only, not by
		 * recomputing from arg.dun + bytes_done, to avoid silent
		 * truncation when bytes_done grows past UINT_MAX data units.
		 */
		bio_crypt_dun_increment(dun, (unsigned int)(bio_bytes >> arg.data_unit_size_bits));
		bytes_done += bio_bytes;
	}
	ret = 0;

out:
	kfree(iov);
	return ret;
}

static long bcp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case BCP_BIND_CONTEXT:
		return bcp_ioctl_bind_context(file, argp);
	case BCP_GET_CRYPTO_CAPS:
		return bcp_ioctl_get_crypto_caps(file, argp);
	case BCP_SUBMIT_IO_BY_VSLOT:
		return bcp_ioctl_submit_io_by_vslot(file, argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations bcp_fops = {
	.owner		= THIS_MODULE,
	.open		= bcp_open,
	.release	= bcp_release,
	.unlocked_ioctl	= bcp_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice bcp_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "blk-crypto-proxy",
	.fops	= &bcp_fops,
};

static int __init blk_crypto_proxy_init(void)
{
	int ret;

	ret = misc_register(&bcp_misc);
	if (ret)
		return ret;
	return 0;
}

static void __exit blk_crypto_proxy_exit(void)
{
	misc_deregister(&bcp_misc);
}

module_init(blk_crypto_proxy_init);
module_exit(blk_crypto_proxy_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Host-side inline crypto proxy for virtio-blk guests");
