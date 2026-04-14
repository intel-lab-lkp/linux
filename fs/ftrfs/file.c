// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — File operations (skeleton)
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * NOTE: read/write use generic_file_* for now.
 * The EDAC/RS layer will intercept at the block I/O level (next iteration).
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include "ftrfs.h"

const struct file_operations ftrfs_file_operations = {
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = filemap_splice_read,
};

const struct inode_operations ftrfs_file_inode_operations = {
	.getattr        = simple_getattr,
};
