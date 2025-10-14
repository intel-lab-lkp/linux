// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019	Sami Tolvanen <samitolvanen@google.com>, Google, Inc.
 * Copyright (C) 2024	Jinghao Jia   <jinghao7@illinois.edu>,   UIUC
 * Copyright (C) 2024	Wentao Zhang  <wentaoz5@illinois.edu>,   UIUC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"llvm-cov: " fmt

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include "llvm-cov.h"

/*
 * This lock guards both counter/bitmap reset and serialization of the
 * raw profile data. Keeping both of these activities separate via locking
 * ensures that we don't try to serialize data that's being reset.
 */
DEFINE_SPINLOCK(llvm_cov_lock);

static struct dentry *directory;

struct llvm_cov_private_data {
	char *buffer;
	unsigned long size;
};

/*
 * Raw profile data format:
 * https://llvm.org/docs/InstrProfileFormat.html#raw-profile-format. We will
 * only populate information that's relevant to basic Source-based Code Coverage
 * before serialization. Other features like binary IDs, continuous mode,
 * single-byte mode, value profiling, type profiling etc are not implemented.
 */

static void llvm_cov_fill_raw_profile_header(void **buffer)
{
	struct __llvm_profile_header *header = *(struct __llvm_profile_header **)buffer;

	header->magic = INSTR_PROF_RAW_MAGIC_64;
	header->version = INSTR_PROF_RAW_VERSION;
	header->binary_ids_size = 0;
	header->num_data = __llvm_prf_data_count();
	header->padding_bytes_before_counters = 0;
	header->num_counters = __llvm_prf_cnts_count();
	header->padding_bytes_after_counters =
		__llvm_prf_get_padding(__llvm_prf_cnts_size());
	header->num_bitmap_bytes = __llvm_prf_bits_size();
	header->padding_bytes_after_bitmap_bytes =
		__llvm_prf_get_padding(__llvm_prf_bits_size());
	header->names_size = __llvm_prf_names_size();
	header->counters_delta = (u64)__llvm_prf_cnts_start -
				 (u64)__llvm_prf_data_start;
	header->bitmap_delta   = (u64)__llvm_prf_bits_start -
				 (u64)__llvm_prf_data_start;
	header->names_delta    = (u64)__llvm_prf_names_start;
#if defined(CONFIG_CC_IS_CLANG) && CONFIG_CLANG_VERSION >= 190000
	header->num_v_tables = 0;
	header->v_names_size = 0;
#endif
	header->value_kind_last = IPVK_LAST;

	*buffer += sizeof(*header);
}

/*
 * Copy the source into the buffer, incrementing the pointer into buffer in the
 * process.
 */
static void llvm_cov_copy_section_to_buffer(void **buffer, void *src,
					    unsigned long size)
{
	memcpy(*buffer, src, size);
	*buffer += size;
}

static unsigned long llvm_cov_get_raw_profile_size(void)
{
	return sizeof(struct __llvm_profile_header) +
	       __llvm_prf_data_size() +
	       __llvm_prf_cnts_size() +
	       __llvm_prf_get_padding(__llvm_prf_cnts_size()) +
	       __llvm_prf_bits_size() +
	       __llvm_prf_get_padding(__llvm_prf_bits_size()) +
	       __llvm_prf_names_size() +
	       __llvm_prf_get_padding(__llvm_prf_names_size());
}

/*
 * Serialize in-memory data into a format LLVM tools can understand
 * (https://llvm.org/docs/InstrProfileFormat.html#raw-profile-format)
 */
static int llvm_cov_serialize_raw_profile(struct llvm_cov_private_data *p)
{
	int err = 0;
	void *buffer;

	p->size = llvm_cov_get_raw_profile_size();
	p->buffer = vzalloc(p->size);

	if (!p->buffer) {
		err = -ENOMEM;
		goto out;
	}

	buffer = p->buffer;

	llvm_cov_fill_raw_profile_header(&buffer);
	llvm_cov_copy_section_to_buffer(&buffer, __llvm_prf_data_start,
					__llvm_prf_data_size());
	llvm_cov_copy_section_to_buffer(&buffer, __llvm_prf_cnts_start,
					__llvm_prf_cnts_size());
	buffer += __llvm_prf_get_padding(__llvm_prf_cnts_size());
	llvm_cov_copy_section_to_buffer(&buffer, __llvm_prf_bits_start,
					__llvm_prf_bits_size());
	buffer += __llvm_prf_get_padding(__llvm_prf_bits_size());
	llvm_cov_copy_section_to_buffer(&buffer, __llvm_prf_names_start,
					__llvm_prf_names_size());
	buffer += __llvm_prf_get_padding(__llvm_prf_names_size());

out:
	return err;
}

/* open() implementation for llvm-cov data file. */
static int llvm_cov_open(struct inode *inode, struct file *file)
{
	struct llvm_cov_private_data *data;
	unsigned long flags;
	int err;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto out;
	}

	flags = llvm_cov_claim_lock();

	err = llvm_cov_serialize_raw_profile(data);
	if (unlikely(err)) {
		kfree(data);
		goto out_unlock;
	}

	file->private_data = data;

out_unlock:
	llvm_cov_release_lock(flags);
out:
	return err;
}

/* read() implementation for llvm-cov data file. */
static ssize_t llvm_cov_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct llvm_cov_private_data *data = file->private_data;

	if (!data)
		return -EBADF;

	return simple_read_from_buffer(buf, count, ppos, data->buffer,
				       data->size);
}

/* release() implementation for llvm-cov data file. */
static int llvm_cov_release(struct inode *inode, struct file *file)
{
	struct llvm_cov_private_data *data = file->private_data;

	if (data) {
		vfree(data->buffer);
		kfree(data);
	}

	return 0;
}

static const struct file_operations llvm_cov_data_fops = {
	.owner		= THIS_MODULE,
	.open		= llvm_cov_open,
	.read		= llvm_cov_read,
	.llseek		= default_llseek,
	.release	= llvm_cov_release
};

/* write() implementation for llvm-cov reset file */
static ssize_t reset_write(struct file *file, const char __user *addr,
			   size_t len, loff_t *pos)
{
	unsigned long flags;

	flags = llvm_cov_claim_lock();
	memset(__llvm_prf_cnts_start, 0, __llvm_prf_cnts_size());
	memset(__llvm_prf_bits_start, 0, __llvm_prf_bits_size());
	llvm_cov_release_lock(flags);

	return len;
}

static const struct file_operations llvm_cov_reset_fops = {
	.owner		= THIS_MODULE,
	.write		= reset_write,
	.llseek		= noop_llseek,
};

/* Create debugfs entries. */
static int __init llvm_cov_init(void)
{
	directory = debugfs_create_dir("llvm-cov", NULL);
	if (!directory)
		goto err_remove;

	if (!debugfs_create_file("profraw", 0400, directory, NULL,
				 &llvm_cov_data_fops))
		goto err_remove;

	if (!debugfs_create_file("reset", 0200, directory, NULL,
				 &llvm_cov_reset_fops))
		goto err_remove;

	return 0;

err_remove:
	debugfs_remove_recursive(directory);
	pr_err("initialization failed\n");
	return -EIO;
}

/* Remove debugfs entries. */
static void __exit llvm_cov_exit(void)
{
	debugfs_remove_recursive(directory);
}

module_init(llvm_cov_init);
module_exit(llvm_cov_exit);
