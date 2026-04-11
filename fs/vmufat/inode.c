// SPDX-License-Identifier: GPL-2.0+
/*
 * VMUFAT file system
 *
 * Copyright (C) 2002-2012, 2025, 2026	Adrian McMenamin
 * Copyright (C) 2002			Paul Mundt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/fs.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/magic.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include "vmufat.h"

const struct inode_operations vmufat_inode_operations;
const struct file_operations vmufat_file_operations;
const struct address_space_operations vmufat_address_space_operations;
const struct file_operations vmufat_file_dir_operations;
extern struct kmem_cache *vmufat_blist_cachep;
/* Linear day numbers of the respective 1sts in non-leap years. */
int day_n[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

static struct dentry *vmufat_inode_lookup(struct inode *in, struct dentry *dent,
	unsigned int ignored)
{
	struct super_block *sb;
	struct memcard *vmudetails;
	struct buffer_head *bh = NULL;
	struct inode *ino;
	int i, j;
	int error = 0;

	if (dent->d_name.len > VMUFAT_NAMELEN) {
		return ERR_PTR(-ENAMETOOLONG);
	}
	sb = in->i_sb;
	vmudetails = sb->s_fs_info;

	for (i = vmudetails->dir_bnum;
		i > vmudetails->dir_bnum - vmudetails->dir_len; i--) {
		brelse(bh);
		bh = vmufat_sb_bread(sb, i);
		if (!bh) {
			return ERR_PTR(-EIO);
		}
		for (j = 0; j < VMU_DIR_ENTRIES_PER_BLOCK; j++) {
			int record_offset = j * VMU_DIR_RECORD_LEN;
			if (bh->b_data[record_offset] == 0)
				goto insert_negative;
			if (memcmp(dent->d_name.name,
				bh->b_data + record_offset +
				VMUFAT_NAME_OFFSET, dent->d_name.len) == 0) {
				ino = vmufat_get_inode(sb,
					le16_to_cpu(((u16 *) bh->b_data)
					[(record_offset / 2)
					+ VMUFAT_FIRSTBLOCK_OFFSET16]));
				if (IS_ERR(ino)) {
					error = PTR_ERR(ino);
					goto out;
				} else if (!ino) {
					error = -EACCES;
					goto out;
				}
				d_add(dent, ino);
				goto out;
			}
		}
	}
insert_negative:
	d_add(dent, NULL); /* Did not find the file */
out:
	brelse(bh);
	return ERR_PTR(error);
}


/* Search for free block in inclusive range */
static int vmufat_get_freeblock(int start, int end, struct buffer_head *bh)
{
	int i, ret = -1;
	__le16 fatdata;
	if (start < end) {
		for (i = start; i <= end; i++) {
			fatdata = le16_to_cpu(((u16 *)bh->b_data)[i]);
			if (fatdata == VMUFAT_UNALLOCATED) {
				ret = i;
				break;
			}
		}
	} else {
		for (i = start; i >= end; i--) {
			fatdata = le16_to_cpu(((u16 *)bh->b_data)[i]);
			if (fatdata == VMUFAT_UNALLOCATED) {
				ret = i;
				break;
			}
		}
	}
	return ret;
}

/*
 * Find a block marked free in the FAT - for exec files
 * Dreamcasts expect data and exec files to be stored differently
 * - we attempt to replicate that with *enforcing* a policy
 */
static int vmufat_find_free_forward(struct super_block *sb)
{
	struct memcard *vmudetails;
	int testblk, fatblk, i;
	struct buffer_head *bh_fat;
	vmudetails = sb->s_fs_info;
	fatblk = vmudetails->fat_bnum;
	for (i = 0; i < vmudetails->fat_len; i++) {
		bh_fat = vmufat_sb_bread(sb, fatblk + i);
		if (!bh_fat) {
			return -EIO;
		}
		testblk = vmufat_get_freeblock(0, VMU_BLK_SZ16 - 1, bh_fat);
		brelse(bh_fat);
		if (testblk >= 0 && testblk + (i * VMU_BLK_SZ16) < 
			vmudetails->numblocks) {
			return testblk + (i * VMU_BLK_SZ16);
		}
	}
	printk(KERN_INFO "VMUFAT: volume is full, cannot save game file\n");
	return -ENOSPC;
}

/* And for data files*/
static int vmufat_find_free_backward(struct super_block *sb)
{
	struct memcard *vmudetails;
	int testblk;
	int i, readblk;
	struct buffer_head *bh_fat;

	vmudetails = sb->s_fs_info;
	readblk = (vmudetails->numblocks - 1) % VMU_BLK_SZ16;

	for (i = vmudetails->fat_len - 1; i >= 0; i--) {
		bh_fat = vmufat_sb_bread(sb, i + vmudetails->fat_bnum);
		if (!bh_fat) {
			return -EIO;
		}
		testblk = vmufat_get_freeblock(readblk, 0, bh_fat);
		brelse(bh_fat);
		if (testblk >= 0) {
			return testblk + (i * VMU_BLK_SZ16);
		}
		readblk = VMU_BLK_SZ16 - 1;
	}
	printk(KERN_INFO "VMUFAT: volume is full, cannot save data file\n");
	return -ENOSPC;
}

/* read the FAT for a given block */
u16 vmufat_get_fat(struct super_block *sb, long block)
{
	struct buffer_head *bufhead;
	int offset;
	u16 block_content = VMUFAT_ERROR;
	struct memcard *vmudetails = sb->s_fs_info;

	/* which block in the FAT */
	offset = block / VMU_BLK_SZ16;
	if (offset >= vmudetails->fat_len)
		goto out;
	/* fat_bnum points to lowest block in FAT */
	bufhead = vmufat_sb_bread(sb, offset + vmudetails->fat_bnum);
	if (!bufhead)
		goto out;
	/* look inside the block */
	block_content = le16_to_cpu(((u16 *)bufhead->b_data)
		[block % VMU_BLK_SZ16]);
	brelse(bufhead);
out:
	return block_content;
}

/* set the FAT for a given block */
static int vmufat_set_fat(struct super_block *sb, long block, u16 data)
{
	struct buffer_head *bh;
	int offset;
	struct memcard *vmudetails = sb->s_fs_info;

	offset = block / VMU_BLK_SZ16;
	if (offset >= vmudetails->fat_len) {
		return -EINVAL;
	}
	bh = vmufat_sb_bread(sb, offset + vmudetails->fat_bnum);
	if (!bh) {
		return -EIO;
	}
	((u16 *) bh->b_data)[block % VMU_BLK_SZ16] = cpu_to_le16(data);
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

/* No real time clock or real time clock read fails */
static void vmufat_save_bcd_no_rtc(struct inode *in, char *bh, int index_to_dir)
{
	u32 years, days, seconds;
	unsigned char bcd_century, nl_day, bcd_month;
	unsigned char u8year;
	time64_t unix_date, substitute_date;
	unix_date = in->i_mtime_sec;
	substitute_date = unix_date;
	do_div(substitute_date, SECONDS_PER_DAY);
	days = (u32)substitute_date; /* cast not an issue for next 5 million years+ */
	years = days / DAYS_PER_YEAR;
	/* 1 Jan gets 1 day later after every leap year */
	if ((years + 3) / 4 + DAYS_PER_YEAR * years >= days)
		years--;
	/* rebase days to account for leap years */
	days -= (years + 3) / 4 + DAYS_PER_YEAR * years;
	// 2100 is not a leap year
	if (years >= CENTURY22 + 1) {
		days--;
	}
	/* 1 Jan is Day 1 */
	days++;
	if (days == (FEB28 + 1) && !(years % 4) && (years != CENTURY22)) {
		nl_day = days;
		bcd_month = 2;
	} else {
		nl_day = (years % 4) || days <= FEB28 ? days : days - 1;
		for (bcd_month = 0; bcd_month < 12; bcd_month++) {
			if (day_n[bcd_month] > nl_day)
				break;
		}
	}

	bcd_century = 19;
	if (years > 29)
		bcd_century += 1 + (years - CENTURY21)/100;

	bh[index_to_dir + VMUFAT_DIR_CENT] = bin2bcd(bcd_century);
	u8year = years + START_OF_EPOCH; /* account for epoch */
	if (u8year > 99)
		u8year = u8year - 100;

	bh[index_to_dir + VMUFAT_DIR_YEAR] = bin2bcd(u8year);
	bh[index_to_dir + VMUFAT_DIR_MONTH] = bin2bcd(bcd_month);
	bh[index_to_dir + VMUFAT_DIR_DAY] =
	    bin2bcd(days - day_n[bcd_month - 1]);
	substitute_date = unix_date;
	do_div(substitute_date, SECONDS_PER_HOUR);
	bh[index_to_dir + VMUFAT_DIR_HOUR] =
	    bin2bcd(do_div(substitute_date, HOURS_PER_DAY));
	seconds = do_div(unix_date, SIXTY_MINS_OR_SECS);
	bh[index_to_dir + VMUFAT_DIR_MIN] =
		bin2bcd(unix_date);
	bh[index_to_dir + VMUFAT_DIR_SEC] =
		bin2bcd(seconds);
}

#ifdef CONFIG_RTC_CLASS
static void vmufat_save_bcd_rtc(struct rtc_device *rtc, struct inode *in,
	char *bh, int index_to_dir)
{
	struct rtc_time now;

	if (rtc_read_time(rtc, &now))
		vmufat_save_bcd_no_rtc(in, bh, index_to_dir);
	else {
		bh[index_to_dir + VMUFAT_DIR_CENT] = bin2bcd((char)((now.tm_year + 1900)/100));
		bh[index_to_dir + VMUFAT_DIR_YEAR] =
			bin2bcd((char)(now.tm_year % 100));
		bh[index_to_dir + VMUFAT_DIR_MONTH] = bin2bcd((char)(now.tm_mon + 1));
		bh[index_to_dir + VMUFAT_DIR_DAY] = bin2bcd((char)(now.tm_mday));
		bh[index_to_dir + VMUFAT_DIR_HOUR] = bin2bcd((char)(now.tm_hour));
		bh[index_to_dir + VMUFAT_DIR_MIN] = bin2bcd((char)(now.tm_min));
		bh[index_to_dir + VMUFAT_DIR_SEC] = bin2bcd((char)(now.tm_sec));
		bh[index_to_dir + VMUFAT_DIR_DOW] = bin2bcd((char)((now.tm_wday + 6))
			% DAYS_PER_WEEK);
	}
}
#endif

/*
 * write out the date in bcd format
 * in the appropriate part of the
 * directory entry
 */
void vmufat_save_bcd(struct inode *in, char *bh, int index_to_dir)
{
#ifdef CONFIG_RTC_CLASS
	struct rtc_device *rtc;
	rtc = rtc_class_open("rtc0");
	if (!rtc)
#endif
		vmufat_save_bcd_no_rtc(in, bh, index_to_dir);
#ifdef CONFIG_RTC_CLASS
	else {
		vmufat_save_bcd_rtc(rtc, in, bh, index_to_dir);
		rtc_class_close(rtc);
	}
#endif
}

static int vmufat_allocate_inode(umode_t imode,
		struct super_block *sb, struct inode *in)
{
	struct vmufat_inode *vin = VMUFAT_I(in);
	int error = 0;
	/* Executable files should be at the start of the volume if possible */
	if (imode & EXEC) {
		vin->ft = GAME;
		if (vmufat_get_fat(sb, 0) != VMUFAT_UNALLOCATED) {
			/* Warning for anyone concerned about playable games */
			printk(KERN_WARNING 
				"VMUFAT: Warning cannot write excutable "
				"file to block 0: already allocated.\n");
		}
		else {
			in->i_ino = VMUFAT_ZEROBLOCK;
			return error;
		}
		error = vmufat_find_free_forward(sb);
	} else {
		vin->ft = DATA;
		error = vmufat_find_free_backward(sb);
	}
	if (error == 0) {
		in->i_ino = VMUFAT_ZEROBLOCK;
	} else if (error > 0) {
		in->i_ino = error;
	}
	return error;
}

static void vmufat_setup_inode(struct inode *in, struct super_block *sb)
{
	simple_inode_init_ts(in);
	in->i_blocks = 1;
	in->i_sb = sb;
	in->i_op = &vmufat_inode_operations;
	in->i_fop = &vmufat_file_operations;
	in->i_mapping->a_ops = &vmufat_address_space_operations;
	insert_inode_hash(in);
}

static void vmu_handle_zeroblock(int recno, struct buffer_head *bh, int ino)
{
	/* offset and header offset settings */
	if (ino != VMUFAT_ZEROBLOCK) {
		((u16 *) bh->b_data)[recno + VMUFAT_START_OFFSET16] =
			cpu_to_le16(ino);
		((u16 *) bh->b_data)[recno + VMUFAT_HEADER_OFFSET16] = 0;
	} else {
		((u16 *) bh->b_data)[recno + VMUFAT_START_OFFSET16] = 0;
		((u16 *) bh->b_data)[recno + VMUFAT_HEADER_OFFSET16] =
			cpu_to_le16(1);
	}
}

static void vmu_write_name(int recno, struct buffer_head *bh, char *name,
	int len)
{
	if ((bh->b_size - recno - VMUFAT_NAME_OFFSET) >= VMUFAT_NAMELEN &&
		(bh->b_size - recno - VMUFAT_NAME_OFFSET) >= len) {
		memset((char *)(bh->b_data + recno + VMUFAT_NAME_OFFSET),
			'\0', VMUFAT_NAMELEN);
		memcpy((char *)(bh->b_data + recno + VMUFAT_NAME_OFFSET),
			name, len);
	}
}

static int vmufat_inode_create(struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *de, umode_t imode, bool excl)
{
	int i, j, entry, error = 0, freeblock;
	struct inode *inode;
	struct super_block *sb;
	struct memcard *vmudetails;
	struct buffer_head *bh = NULL;

	sb = dir->i_sb;
	vmudetails = sb->s_fs_info;
	inode = new_inode(sb);
	if (!inode) {
		error = -ENOSPC;
		goto out;
	}

	mutex_lock(&vmudetails->mutex);
	freeblock = vmufat_allocate_inode(imode, sb, inode);
	if (freeblock < 0) {
		mutex_unlock(&vmudetails->mutex);
		error = freeblock;
		goto clean_inode;
	}
	/* mark as single block file - may grow later */
	error = vmufat_set_fat(sb, freeblock, VMUFAT_FILE_END);
	mutex_unlock(&vmudetails->mutex);
	if (error)
		goto clean_inode;
	inode_init_owner(&nop_mnt_idmap, inode, dir, imode);
	vmufat_setup_inode(inode, sb);

	/* Write to the directory
	 * Now search for space for the directory entry */
	mutex_lock(&vmudetails->mutex);
	for (i = vmudetails->dir_bnum;
		i > vmudetails->dir_bnum - vmudetails->dir_len; i--) {
		brelse(bh);
		bh = vmufat_sb_bread(sb, i);
		if (!bh) {
			mutex_unlock(&vmudetails->mutex);
			error = -EIO;
			goto clean_fat;
		}
		for (j = 0; j < VMU_DIR_ENTRIES_PER_BLOCK; j++) {
			entry = j * VMU_DIR_RECORD_LEN;
			if (((bh->b_data)[entry]) == 0) {
				goto dir_space_found;
			}
		}
	}
	goto clean_fat;
dir_space_found:
	/* Have the directory entry
	 * so now update it */
	if (imode & EXEC)
		bh->b_data[entry] = VMU_GAME;
	else
		bh->b_data[entry] = VMU_DATA;

	/* copy protection settings */
	if (bh->b_data[entry + 1] != (char) NOCOPY)
		bh->b_data[entry + 1] = (char) CANCOPY;

	vmu_handle_zeroblock(entry / 2, bh, inode->i_ino);
	vmu_write_name(entry, bh, (char *) de->d_name.name, de->d_name.len);

	/* BCD timestamp it */
	vmufat_save_bcd(inode, bh->b_data, entry);

	((u16 *) bh->b_data)[entry / 2 + VMUFAT_SIZE_OFFSET16] =
	    cpu_to_le16(inode->i_blocks);
	mark_buffer_dirty(bh);
	brelse(bh);

	error = vmufat_list_blocks(inode);
	if (error)
		goto clean_fat;
	mutex_unlock(&vmudetails->mutex);
	mark_inode_dirty(inode);
	d_instantiate(de, inode);
	return error;

clean_fat:
	vmufat_set_fat(sb, freeblock, VMUFAT_UNALLOCATED);
	mutex_unlock(&vmudetails->mutex);
clean_inode:
	iput(inode);
out:
	if (error < 0)
		printk(KERN_ERR "VMUFAT: inode creation fails with error"
			" %i\n", error);
	return error;
}

static int vmufat_readdir(struct file *filp, struct dir_context *ctx)
{
	int filenamelen, index, j, k;
	int error = 0;
	struct vmufat_file_info *saved_file = NULL;
	struct dentry *dentry;
	struct inode *inode;
	struct super_block *sb;
	struct memcard *vmudetails;
	struct buffer_head *bh = NULL;

	inode = file_inode(filp);
	dentry = filp->f_path.dentry;
	sb = inode->i_sb;
	vmudetails = sb->s_fs_info;
	index = ctx->pos;
	/* handle . for this directory and .. for parent */
	switch ((unsigned int) ctx->pos) {
	case 0:
		error = ctx->actor(ctx, ".", 1, index++, inode->i_ino, DT_DIR);
		ctx->pos++;
		if (error < 0)
			return error;
		fallthrough;
	case 1:
		error = ctx->actor(ctx, "..", 2, index++,
			    dentry->d_parent->d_inode->i_ino, DT_DIR);
		ctx->pos++;
		if (error < 0)
			return error;
	default:
		break;
	}

	saved_file =
	    kmalloc(sizeof(struct vmufat_file_info), GFP_KERNEL);
	if (!saved_file) {
		return -ENOMEM;
	}

	for (j = vmudetails->dir_bnum -
		(index - 2) / VMU_DIR_ENTRIES_PER_BLOCK;
		j > vmudetails->dir_bnum - vmudetails->dir_len; j--) {
		brelse(bh);
		bh = vmufat_sb_bread(sb, j);
		if (!bh) {
			return -EIO;
		}
		for (k = (index - 2) % VMU_DIR_ENTRIES_PER_BLOCK;
			k < VMU_DIR_ENTRIES_PER_BLOCK; k++) {
			int pos, pos16;
			pos = k * VMU_DIR_RECORD_LEN;
			pos16 = k * VMU_DIR_RECORD_LEN16;
			saved_file->ftype = bh->b_data[pos];
			if (saved_file->ftype == 0)
				goto finish;
			saved_file->fblk =
				le16_to_cpu(((u16 *) bh->b_data)
				[pos16 + VMUFAT_START_OFFSET16]);
			if (saved_file->fblk == 0)
				saved_file->fblk = VMUFAT_ZEROBLOCK;
			if ((bh->b_size - pos - VMUFAT_NAME_OFFSET) >=
				VMUFAT_NAMELEN) {
				memcpy(saved_file->fname,
				bh->b_data + pos + VMUFAT_NAME_OFFSET,
				VMUFAT_NAMELEN);
			}
			filenamelen = strnlen(saved_file->fname,
						VMUFAT_NAMELEN);
			error = ctx->actor(ctx, saved_file->fname, filenamelen,
				index++, saved_file->fblk, DT_REG);
			ctx->pos++;
			if (error < 0)
				goto finish;
		}
	}
finish:
	ctx->pos = index;
	kfree(saved_file);
	brelse(bh);
	return error;
}


int vmufat_list_blocks(struct inode *in)
{
	struct vmufat_inode *vi;
	struct super_block *sb;
	long nextblock;
	long ino;
	struct memcard *vmudetails;
	int error = -EINVAL;
	struct vmufat_block *iter, *iter2, *vbl;
	u16 fatdata;

	vi = VMUFAT_I(in);
	if (!vi)
		return error;
	sb = in->i_sb;
	ino = in->i_ino;
	vmudetails = sb->s_fs_info;
	error = 0;
	nextblock = ino;
	if (nextblock == VMUFAT_ZEROBLOCK)
		nextblock = 0;

	/* Delete any previous list of blocks */
	list_for_each_entry_safe(iter, iter2, &vi->blocks, b_list) {
		list_del(&iter->b_list);
		kmem_cache_free(vmufat_blist_cachep, iter);
	}
	vi->nblcks = 0;
	do {
		vbl = kmem_cache_alloc(vmufat_blist_cachep,
			GFP_KERNEL);
		if (!vbl) {
			error = -ENOMEM;
			goto unwind_out;
		}
		INIT_LIST_HEAD(&vbl->b_list);
		vbl->bno = nextblock;
		list_add_tail(&vbl->b_list, &vi->blocks);
		vi->nblcks++;

		/* Find next block in the FAT - if there is one */
		fatdata = vmufat_get_fat(sb, nextblock);
		if (fatdata == VMUFAT_UNALLOCATED) {
			printk(KERN_ERR "VMUFAT: FAT table appears to have"
				" been corrupted.\n");
			error = -EIO;
			goto unwind_out;
		}
		if (fatdata == VMUFAT_FILE_END)
			break;
		nextblock = fatdata;
	} while (1);
	return error;

unwind_out:
	list_for_each_entry_safe(iter, iter2, &vi->blocks, b_list) {
		list_del(&iter->b_list);
		kmem_cache_free(vmufat_blist_cachep, iter);
	}
	return error;
}

static int vmufat_clean_fat(struct super_block *sb, int inum)
{
	int error = 0;
	u16 fatword, nextword;

	nextword = inum;
	do {
		fatword = vmufat_get_fat(sb, nextword);
		if (fatword == VMUFAT_ERROR) {
			error = -EIO;
			printk(KERN_ERR
				"VMUFAT: Failure while cleaning FAT.\n");
			break;
		}
		error = vmufat_set_fat(sb, nextword, VMUFAT_UNALLOCATED);
		if (error)
			break;
		if (fatword == VMUFAT_FILE_END)
			break;
		nextword = fatword;
	} while (1);

	return error;
}

/*
 * Delete inode by marking space as free in FAT
 * no need to waste time and effort by actually
 * wiping underlying data on media
 */
static void vmufat_remove_inode(struct inode *in)
{
	struct buffer_head *bh = NULL, *bh_old = NULL;
	struct super_block *sb;
	struct memcard *vmudetails;
	int i, j, k, l, x;
	bool found, first = true;

	if (in->i_ino == VMUFAT_ZEROBLOCK)
		in->i_ino = 0;
	sb = in->i_sb;
	vmudetails = sb->s_fs_info;
	if (in->i_ino > vmudetails->fat_len * sb->s_blocksize / 2) {
		printk(KERN_ERR "VMUFAT: attempting to delete"
			"inode beyond device size");
		return;
	}

	mutex_lock(&vmudetails->mutex);
	if (vmufat_clean_fat(sb, in->i_ino)) {
		mutex_unlock(&vmudetails->mutex);
		goto failure;
	}

	/* Now clean the directory entry
	 * Have to walk through entries
	 * to find the appropriate entry */
	for (x = vmudetails->dir_bnum;
		x > vmudetails->dir_bnum - vmudetails->dir_len; x--) {
		brelse(bh);
		bh = vmufat_sb_bread(sb, x);
		if (!bh) {
			mutex_unlock(&vmudetails->mutex);
			goto failure;
		}
		for (j = 0; j < VMU_DIR_ENTRIES_PER_BLOCK; j++) {
			if (bh->b_data[j * VMU_DIR_RECORD_LEN] == 0) {
				mutex_unlock(&vmudetails->mutex);
				brelse(bh);
				goto failure;
			}
			if (le16_to_cpu(((u16 *) bh->b_data)
				[j * VMU_DIR_RECORD_LEN16 +
				VMUFAT_FIRSTBLOCK_OFFSET16]) == in->i_ino) {
				found = true;
				goto found_bk;
			}
		}
	}
found_bk:
	// actually found nothing - an error or failure of some sort
	if (!found) {
		brelse(bh);
		goto failure;
	}
	// have found the directory entry, so first we clean it out
	for (i = 0; i < VMU_DIR_RECORD_LEN; i++) {
		bh->b_data[i + (j * VMU_DIR_RECORD_LEN)] = 0;
	}
	mark_buffer_dirty(bh);
	// now test if there are further records
	found = false;
	for (i = x; i > vmudetails->dir_bnum - vmudetails->dir_len; i--) {
		brelse(bh_old);
		bh_old = vmufat_sb_bread(sb, i);
		if (first) {
			for (l = j; l < VMU_DIR_ENTRIES_PER_BLOCK; l++) {
				if (first) {
					first = false;
					continue;
				}
				if (bh_old->b_data[l * VMU_DIR_RECORD_LEN]
					== 0) {
					found = true;
					goto found_final;
				}
			}
		} else {
			for (l = 0; l < VMU_DIR_ENTRIES_PER_BLOCK; l++) {
				if (bh_old->b_data[l * VMU_DIR_RECORD_LEN]
					== 0) {
					found = true;
					goto found_final;
				}
			}
		}
	}
found_final:
	if (!found) {
		// cleaned entry was final entry
		mutex_unlock(&vmudetails->mutex);
		brelse(bh_old);
		brelse(bh);
		return;
	}
	// copy the final entry into the empty slot and zero the final entry
	if (l == VMU_DIR_ENTRIES_PER_BLOCK) {
		l = 0;
		brelse(bh_old);
		bh_old = vmufat_sb_bread(sb, i - 1);
	} else {
		l--;
	}
	for (k = 0; k < VMU_DIR_RECORD_LEN; k++) {
		bh->b_data[k + (j * VMU_DIR_RECORD_LEN)] =
			bh_old->b_data[k + (l * VMU_DIR_RECORD_LEN)];
		bh_old->b_data[k + (l * VMU_DIR_RECORD_LEN)] = 0;
	}
	mark_buffer_dirty(bh);
	mark_buffer_dirty(bh_old);
	mutex_unlock(&vmudetails->mutex);
	brelse(bh);
	brelse(bh_old);
	return;

failure:
	printk(KERN_WARNING "VMUFAT: Failure to read volume,"
		" could not delete inode - filesystem may be damaged\n");
	return;
}

/*
 * vmufat_unlink - delete a file pointed to
 * by the dentry (only one directory in a
 * vmufat fs so safe to ignore the inode
 * supplied here
 */
static int vmufat_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *in;
	in = dentry->d_inode;
	vmufat_remove_inode(in);
	return 0;
}

/* Update the directory record */
static int vmufat_increment_filesize(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = NULL;
	struct memcard *vmudetails = sb->s_fs_info;
	unsigned long ino_num = inode->i_ino;
	int error = 0;

	if (ino_num == VMUFAT_ZEROBLOCK) {
		ino_num = 0;
	}

	for (int i = vmudetails->dir_bnum;
		i > vmudetails->dir_bnum - vmudetails->dir_len; i--) {
		brelse(bh);
		bh = vmufat_sb_bread(sb, i);
		if (!bh) {
			error = -EIO;
			return error;
		}
		for (int j = 0; j < VMU_DIR_ENTRIES_PER_BLOCK; j++) {
			int record_offset = j * VMU_DIR_RECORD_LEN;
			if (bh->b_data[record_offset] == 0) {
				continue;
			}
			if (le16_to_cpu(((u16 *) bh->b_data)
				[(j * VMU_DIR_RECORD_LEN16) +
				VMUFAT_FIRSTBLOCK_OFFSET16]) != ino_num) {
					continue;
			}
			int current_count = le16_to_cpu(((u16 *) bh->b_data)
				[(j * VMU_DIR_RECORD_LEN16) + VMUFAT_SIZE_OFFSET16]);
			current_count++;
			((u16 *) bh->b_data)
				[(j * VMU_DIR_RECORD_LEN16) + VMUFAT_SIZE_OFFSET16] =
				cpu_to_le16(current_count);
			mark_buffer_dirty(bh);
			goto done;
		}
	}
done:
	brelse(bh);
	return error;
}

static int vmufat_get_block(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	struct vmufat_inode *vin;
	struct list_head *vlist;
	struct vmufat_block *vblk;
	struct super_block *sb;
	struct memcard *vmudetails;
	int cural;
	int finblk, nxtblk, exeblk;
	struct list_head *iter;
	sector_t cntdwn = iblock;
	sector_t phys;
	int error = -EINVAL;

	vin = VMUFAT_I(inode);
	if (!vin || vin->nblcks <= 0)
		return -EINVAL;
	vlist = &vin->blocks;
	sb = inode->i_sb;
	vmudetails = sb->s_fs_info;

	if (iblock < vin->nblcks) {
		/* block is already here so read it into the buffer head */
		list_for_each(iter, vlist) {
			if (cntdwn-- == 0)
				break;
		}
		vblk = list_entry(iter, struct vmufat_block, b_list);
		clear_buffer_new(bh_result);
		error = 0;
		phys = vblk->bno;
		goto got_it;
	}
	if (!create)
		goto out;
	/*
	 * check not looking for a block too far
	 * beyond the end of the existing file
	 */
	if (iblock > vin->nblcks)
		goto out;

	/* if looking for a block that is not current - allocate it */
	cural = vin->nblcks;
	list_for_each(iter, vlist) {
		if (cural-- == 1)
			break;
	}
	vblk = list_entry(iter, struct vmufat_block, b_list);
	finblk = vblk->bno;

	mutex_lock(&vmudetails->mutex);
	/* Exec files have to be linear on real VMUs */
	/* But this is policy so we warn but don't fail */
	if (inode->i_ino == 0) {
		exeblk = vmufat_get_fat(sb, finblk + 1);
		if (exeblk != VMUFAT_UNALLOCATED) {
			mutex_unlock(&vmudetails->mutex);
			printk(KERN_WARNING "VMUFAT: Cannot allocate linear "
				"space needed for executible\n");
			mutex_lock(&vmudetails->mutex);
			nxtblk = vmufat_find_free_forward(sb);
			if (nxtblk < 0) {
				mutex_unlock(&vmudetails->mutex);
				return nxtblk;
			}
		}
		else {
			nxtblk = finblk + 1;
		}
	} else {
		if (vin->ft == GAME) {
			nxtblk = vmufat_find_free_forward(sb);
		} else {
			nxtblk = vmufat_find_free_backward(sb);
		}
		if (nxtblk < 0) {
			mutex_unlock(&vmudetails->mutex);
			return nxtblk;
		}
	}
	error = vmufat_set_fat(sb, finblk, nxtblk);
	if (!error) {
		error = vmufat_increment_filesize(inode);
	}
	if (error) {
		mutex_unlock(&vmudetails->mutex);
		return error;
	}
	error = vmufat_set_fat(sb, nxtblk, VMUFAT_FILE_END);
	mutex_unlock(&vmudetails->mutex);
	if (error)
		return error;
	error = vmufat_list_blocks(inode);
	mark_inode_dirty(inode);
	if (error)
		return error;
	set_buffer_new(bh_result);
	phys = nxtblk;
	error = 0;
got_it:
	map_bh(bh_result, sb, phys);
out:
	return error;
}

static int vmufat_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, vmufat_get_block);
}

static int vmufat_write_begin(const struct kiocb *iocb, struct address_space *mapping,
		loff_t pos, unsigned len, struct folio **foliop, void **fsdata)
{
	*foliop = NULL;
	return block_write_begin(mapping, pos, len, foliop, vmufat_get_block);
}

static int vmufat_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, vmufat_get_block);
}

static int vmufat_getattr(struct mnt_idmap *idmap, const struct path *path,
	struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct super_block *sb;
	struct memcard *vmudetails;
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	/* correct for superblock to give directory size */
	/* as all metadata combined */
	sb = inode->i_sb;
	if (sb) {
		vmudetails = sb->s_fs_info;
		if (vmudetails && vmudetails->sb_bnum == inode->i_ino) {
			stat->size = (vmudetails->fat_len + vmudetails->dir_len + 1) * 
				VMU_BLK_SZ;
			stat->nlink = vmufat_count_files(sb) + 2;
		}
	}
	return 0;
}

const struct address_space_operations
		vmufat_address_space_operations = {
	.read_folio =	vmufat_read_folio,
	.writepages =	vmufat_writepages,
	.write_begin =	vmufat_write_begin,
	.write_end =	generic_write_end,
};

const struct inode_operations vmufat_inode_operations = {
	.lookup =	vmufat_inode_lookup,
	.create =	vmufat_inode_create,
	.unlink =	vmufat_unlink,
	.getattr =	vmufat_getattr,
};

const struct file_operations vmufat_file_dir_operations = {
	.read =			generic_read_dir,
	.iterate_shared =	vmufat_readdir,
	.fsync =		generic_file_fsync,
	.llseek =		generic_file_llseek,
};

const struct file_operations vmufat_file_operations = {
	.llseek =	generic_file_llseek,
	.read_iter =	generic_file_read_iter,
	.write_iter =	generic_file_write_iter,
	.fsync =	generic_file_fsync,
};
