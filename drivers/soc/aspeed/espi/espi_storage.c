// SPDX-License-Identifier: GPL-2.0+
/*
 * eSPI TAFS back storage interface
 */

#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "espi_storage.h"

/*
 * aspeed_espi_lun: use an existing block device or file as backend storage for eSPI flash channel.
 *
 * Typical path:
 *   host <-> eSPI bus <-> eSPI slave protocol
 *        <-> aspeed_espi_lun_*() helpers
 *        <-> backend block device (e.g., /dev/mmcblk0p3)
 */

int aspeed_espi_lun_open(struct aspeed_espi_lun *lun, const char *path,
			 bool initially_ro, bool cdrom)
{
	unsigned int blksize, blkbits;
	loff_t size, num_sectors;
	struct inode *inode;
	struct file *filp;
	int ro;
	int rc;

	filp = NULL;
	ro = initially_ro;

	if (!lun || !path) {
		pr_err("espi_lun_open: invalid args lun=%p path=%p\n", lun,
		       path);
		return -EINVAL;
	}

	pr_info("espi_lun_open: path=%s ro=%d cdrom=%d\n", path, ro, cdrom);

	/* Try R/W first, fallback to R/O if failed */
	if (!ro) {
		filp = filp_open(path, O_RDWR | O_LARGEFILE, 0);
		if (PTR_ERR(filp) == -EROFS || PTR_ERR(filp) == -EACCES) {
			pr_err("espi_lun_open: open rw failed rc=%ld, back to ro\n",
			       PTR_ERR(filp));
			ro = 1;
		}
	}

	if (ro) {
		filp = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		if (IS_ERR_OR_NULL(filp)) {
			rc = filp ? PTR_ERR(filp) : -ENODEV;
			pr_err("espi_lun_open: open ro failed rc=%d\n", rc);
			return rc;
		}
		pr_info("espi_lun_open: open ro ok filp=%p\n", filp);
	}

	if (!(filp->f_mode & FMODE_WRITE))
		ro = 1;

	pr_info("espi_lun_open: filp=%p f_inode=%p f_mode=0x%x\n", filp,
		filp ? filp->f_inode : NULL, filp ? filp->f_mode : 0);

	inode = filp->f_mapping->host;

	pr_info("espi_lun_open: inode=%p mode=0%o\n", inode,
		inode ? inode->i_mode : 0);
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode)) {
		rc = -EINVAL;
		goto out_put;
	}

	if (!(filp->f_mode & FMODE_CAN_READ)) {
		rc = -EACCES;
		goto out_put;
	}
	if (!(filp->f_mode & FMODE_CAN_WRITE))
		ro = true;

	size = i_size_read(inode);
	if (size < 0) {
		pr_info("unable to find file size: %s\n", path);
		rc = (int)size;
		goto out_put;
	}

	pr_info("espi_lun_open: size=%lld\n", size);

	if (cdrom) {
		blksize = 2048;
		blkbits = 11;
	} else if (S_ISBLK(inode->i_mode)) {
		blksize = bdev_logical_block_size(I_BDEV(inode));
		pr_info("espi_lun_open: blksize=%d\n", blksize);
		blkbits = blksize_bits(blksize);
		pr_info("espi_lun_open: bdev=%d\n", blkbits);
	} else {
		blksize = 512;
		blkbits = 9;
	}

	pr_info("espi_lun_open: blksize=%u blkbits=%u\n", blksize, blkbits);
	num_sectors = size >> blkbits;
	if (num_sectors < 1) {
		pr_info("file too small: %s\n", path);
		rc = -ETOOSMALL;
		goto out_put;
	}

	lun->blksize = blksize;
	lun->blkbits = blkbits;
	lun->ro = ro;
	lun->filp = filp;
	lun->file_length = size;
	lun->num_sectors = num_sectors;

	lun->cdrom = cdrom;

	return 0;

out_put:
	fput(filp);
	return rc;
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_open);

void aspeed_espi_lun_close(struct aspeed_espi_lun *lun)
{
	if (!lun)
		return;

	if (lun->filp) {
		fput(lun->filp);
		lun->filp = NULL;
	}
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_close);

int aspeed_espi_lun_rw(struct aspeed_espi_lun *lun, bool write, sector_t sector,
		       unsigned int nsect, void *buf)
{
	ssize_t done;
	loff_t pos;
	size_t len;

	if (!lun || !lun->filp || !buf)
		return -EINVAL;

	if (write && lun->ro)
		return -EROFS;

	if (sector >= lun->num_sectors || nsect > lun->num_sectors - sector)
		return -EINVAL;

	pos = (loff_t)sector << lun->blkbits;
	len = (size_t)nsect << lun->blkbits;

	if (write)
		done = kernel_write(lun->filp, buf, len, &pos);
	else
		done = kernel_read(lun->filp, buf, len, &pos);

	if (done != len)
		return done < 0 ? (int)done : -EIO;

	return 0;
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_rw);

int aspeed_espi_lun_read(struct aspeed_espi_lun *lun, sector_t sector,
			 unsigned int nsect, void *buf)
{
	return aspeed_espi_lun_rw(lun, false, sector, nsect, buf);
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_read);

int aspeed_espi_lun_write(struct aspeed_espi_lun *lun, sector_t sector,
			  unsigned int nsect, void *buf)
{
	return aspeed_espi_lun_rw(lun, true, sector, nsect, buf);
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_write);

/*
 * Byte-granular R/W, for eSPI protocol handler use:
 *  - addr/len are in bytes, allowing arbitrary unaligned access
 *  - internally handles sector alignment and RMW
 */
int aspeed_espi_lun_rw_bytes(struct aspeed_espi_lun *lun, bool write, u32 addr,
			     u32 len, u8 *buf)
{
	u32 blk_mask;
	u32 blksize;
	u8 *bounce;
	u32 done;
	int rc;

	done = 0;
	rc = 0;

	if (!lun || !lun->filp || !buf)
		return -EINVAL;

	if (!len)
		return 0;

	if (write && lun->ro)
		return -EROFS;

	blksize = lun->blksize;
	blk_mask = blksize - 1;

	bounce = kmalloc(blksize, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < len) {
		u32 cur_addr = addr + done;
		u32 off_in_blk = cur_addr & blk_mask;
		sector_t sector = cur_addr >> lun->blkbits;
		u32 bytes_this;
		u8 *p = buf + done;

		/* Process up to the end of this sector */
		bytes_this = blksize - off_in_blk;
		if (bytes_this > (len - done))
			bytes_this = len - done;

		if (!off_in_blk && bytes_this == blksize) {
			/* Fully aligned sector, use sector API directly */
			if (write)
				rc = aspeed_espi_lun_write(lun, sector, 1, p);
			else
				rc = aspeed_espi_lun_read(lun, sector, 1, p);
		} else {
			/* partial sector: read one sector first, then overwrite/extract part */
			rc = aspeed_espi_lun_read(lun, sector, 1, bounce);
			if (rc)
				break;

			if (write) {
				memcpy(bounce + off_in_blk, p, bytes_this);
				rc = aspeed_espi_lun_write(lun, sector, 1,
							   bounce);
			} else {
				memcpy(p, bounce + off_in_blk, bytes_this);
			}
		}

		if (rc)
			break;

		done += bytes_this;
	}

	kfree(bounce);
	return rc;
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_rw_bytes);

/*
 * Erase: specify range in bytes, implemented as writing 0xFF pattern.
 * For eSPI ERASE command use.
 */
int aspeed_espi_lun_erase_bytes(struct aspeed_espi_lun *lun, u32 addr, u32 len)
{
	u8 *pattern;
	u32 chunk;
	u32 done;
	int rc;

	done = 0;
	rc = 0;

	if (!lun || !lun->filp)
		return -EINVAL;

	if (!len)
		return 0;

	if (lun->ro)
		return -EROFS;

	chunk = lun->blksize;
	if (chunk > len)
		chunk = len;

	pattern = kmalloc(chunk, GFP_KERNEL);
	if (!pattern)
		return -ENOMEM;

	while (done < len) {
		u32 this_len = len - done;

		if (this_len > chunk)
			this_len = chunk;

		memset(pattern, 0xff, this_len);

		rc = aspeed_espi_lun_rw_bytes(lun, true, addr + done, this_len,
					      pattern);
		if (rc)
			break;

		done += this_len;
	}

	kfree(pattern);
	return rc;
}
EXPORT_SYMBOL_GPL(aspeed_espi_lun_erase_bytes);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ASPEED eSPI slave storage backend helpers");
MODULE_AUTHOR("ASPEED");
