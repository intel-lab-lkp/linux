// SPDX-License-Identifier: GPL-2.0

#include <linux/iversion.h>
#include "ctree.h"
#include "accessors.h"
#include "btrfs_inode.h"
#include "disk-io.h"
#include "ioctl.h"
#include "fs.h"
#include "fscrypt.h"
#include "ioctl.h"
#include "messages.h"
#include "root-tree.h"
#include "super.h"
#include "transaction.h"
#include "volumes.h"
#include "xattr.h"

/*
 * From a given location in a leaf, read a name into a qstr (usually a
 * fscrypt_name's disk_name), allocating the required buffer. Used for
 * nokey names.
 */
int btrfs_fscrypt_get_disk_name(struct extent_buffer *leaf,
				struct btrfs_dir_item *dir_item,
				struct fscrypt_str *name)
{
	unsigned long de_name_len = btrfs_dir_name_len(leaf, dir_item);
	unsigned long de_name = (unsigned long)(dir_item + 1);
	/*
	 * For no-key names, we use this opportunity to find the disk
	 * name, so future searches don't need to deal with nokey names
	 * and we know what the encrypted size is.
	 */
	name->name = kmalloc(de_name_len, GFP_NOFS);

	if (!name->name)
		return -ENOMEM;

	read_extent_buffer(leaf, name->name, de_name, de_name_len);

	name->len = de_name_len;
	return 0;
}

/*
 * This function is extremely similar to fscrypt_match_name() but uses an
 * extent_buffer.
 */
bool btrfs_fscrypt_match_name(struct fscrypt_name *fname,
			      struct extent_buffer *leaf, unsigned long de_name,
			      u32 de_name_len)
{
	const struct fscrypt_nokey_name *nokey_name =
		(const struct fscrypt_nokey_name *)fname->crypto_buf.name;
	u8 digest[SHA256_DIGEST_SIZE];

	if (likely(fname->disk_name.name)) {
		if (de_name_len != fname->disk_name.len)
			return false;
		return !memcmp_extent_buffer(leaf, fname->disk_name.name,
					     de_name, de_name_len);
	}

	if (de_name_len <= sizeof(nokey_name->bytes))
		return false;

	if (memcmp_extent_buffer(leaf, nokey_name->bytes, de_name,
				 sizeof(nokey_name->bytes)))
		return false;

	extent_buffer_sha256(leaf, de_name + sizeof(nokey_name->bytes),
			     de_name_len - sizeof(nokey_name->bytes), digest);

	return !memcmp(digest, nokey_name->sha256, sizeof(digest));
}

static int btrfs_fscrypt_get_context(struct inode *inode, void *ctx, size_t len)
{
	struct btrfs_key key = {
		.objectid = btrfs_ino(BTRFS_I(inode)),
		.type = BTRFS_FSCRYPT_CTX_ITEM_KEY,
		.offset = 0,
	};
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	unsigned long ptr;
	int ret;


	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(NULL, BTRFS_I(inode)->root, &key, path, 0, 0);
	if (ret) {
		len = -ENOENT;
		goto out;
	}

	leaf = path->nodes[0];
	ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);
	/* fscrypt provides max context length, but it could be less */
	len = min_t(size_t, len, btrfs_item_size(leaf, path->slots[0]));
	read_extent_buffer(leaf, ctx, ptr, len);

out:
	btrfs_free_path(path);
	return len;
}

static int btrfs_fscrypt_set_context(struct inode *inode, const void *ctx,
				     size_t len, void *fs_data)
{
	struct btrfs_trans_handle *trans = fs_data;
	struct btrfs_key key = {
		.objectid = btrfs_ino(BTRFS_I(inode)),
		.type = BTRFS_FSCRYPT_CTX_ITEM_KEY,
		.offset = 0,
	};
	struct btrfs_path *path = NULL;
	struct extent_buffer *leaf;
	unsigned long ptr;
	int ret;

	if (!trans)
		trans = btrfs_start_transaction(BTRFS_I(inode)->root, 2);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out_err;
	}

	ret = btrfs_search_slot(trans, BTRFS_I(inode)->root, &key, path, 0, 1);
	if (ret < 0)
		goto out_err;

	if (ret > 0) {
		btrfs_release_path(path);
		ret = btrfs_insert_empty_item(trans, BTRFS_I(inode)->root, path, &key, len);
		if (ret)
			goto out_err;
	}

	leaf = path->nodes[0];
	ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);

	len = min_t(size_t, len, btrfs_item_size(leaf, path->slots[0]));
	write_extent_buffer(leaf, ctx, ptr, len);
	btrfs_mark_buffer_dirty(trans, leaf);
	btrfs_release_path(path);

	if (fs_data)
		return ret;

	BTRFS_I(inode)->flags |= BTRFS_INODE_ENCRYPT;
	btrfs_sync_inode_flags_to_i_flags(inode);
	inode_inc_iversion(inode);
	inode_set_ctime_current(inode);
	ret = btrfs_update_inode(trans, BTRFS_I(inode));
	if (ret)
		goto out_abort;
	btrfs_free_path(path);
	btrfs_end_transaction(trans);
	return 0;
out_abort:
	btrfs_abort_transaction(trans, ret);
out_err:
	if (!fs_data)
		btrfs_end_transaction(trans);
	btrfs_free_path(path);
	return ret;
}

static bool btrfs_fscrypt_empty_dir(struct inode *inode)
{
	return inode->i_size == BTRFS_EMPTY_DIR_SIZE;
}

static struct block_device **btrfs_fscrypt_get_devices(struct super_block *sb,
						       unsigned int *num_devs)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(sb);
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	int nr_devices = fs_devices->open_devices;
	struct block_device **devs;
	struct btrfs_device *device;
	int i = 0;

	devs = kmalloc_array(nr_devices, sizeof(*devs), GFP_NOFS | GFP_NOWAIT);
	if (!devs)
		return ERR_PTR(-ENOMEM);

	rcu_read_lock();
	list_for_each_entry_rcu(device, &fs_devices->devices, dev_list) {
		if (!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA,
						&device->dev_state) ||
		    !device->bdev ||
		    test_bit(BTRFS_DEV_STATE_REPLACE_TGT, &device->dev_state))
			continue;

		devs[i++] = device->bdev;

		if (i >= nr_devices)
			break;

	}
	rcu_read_unlock();

	*num_devs = i;
	return devs;
}

const struct fscrypt_operations btrfs_fscrypt_ops = {
	.get_context = btrfs_fscrypt_get_context,
	.set_context = btrfs_fscrypt_set_context,
	.empty_dir = btrfs_fscrypt_empty_dir,
	.get_devices = btrfs_fscrypt_get_devices,
	.key_prefix = "btrfs:"
};
