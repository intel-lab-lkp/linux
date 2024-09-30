// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Western Digital Corporation or its affiliates.
 */

#include "../fs.h"
#include "../disk-io.h"
#include "../transaction.h"
#include "../volumes.h"
#include "../raid-stripe-tree.h"
#include "btrfs-tests.h"

#define RST_TEST_NUM_DEVICES	2
#define RST_TEST_RAID1_TYPE	(BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_RAID1)

typedef int (*test_func_t)(struct btrfs_trans_handle *trans);

static struct btrfs_device *btrfs_device_by_devid(struct btrfs_fs_devices *fs_devices,
						  u64 devid)
{
	struct btrfs_device *dev;

	list_for_each_entry(dev, &fs_devices->devices, dev_list) {
		if (dev->devid == devid)
			return dev;
	}

	return NULL;
}

static int test_create_update_delete(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_io_context *bioc;
	struct btrfs_io_stripe io_stripe = { };
	u64 map_type = RST_TEST_RAID1_TYPE;
	u64 logical = SZ_1M;
	u64 len = SZ_64K;
	int ret;

	bioc = alloc_btrfs_io_context(fs_info, logical, RST_TEST_NUM_DEVICES);
	if (!bioc) {
		ret = -ENOMEM;
		goto out;
	}

	io_stripe.dev = btrfs_device_by_devid(fs_info->fs_devices, 0);

	for (int i = 0; i < RST_TEST_NUM_DEVICES; i++) {
		struct btrfs_io_stripe *stripe = &bioc->stripes[i];
		struct btrfs_device *dev;

		dev = btrfs_device_by_devid(fs_info->fs_devices, i);
		if (!dev) {
			ret = -EINVAL;
			goto out;
		}

		stripe->dev = dev;
		stripe->physical = logical + i * SZ_1G;
	}

	ret = btrfs_insert_one_raid_extent(trans, bioc);
	if (ret)
		goto out;

	io_stripe.dev = btrfs_device_by_devid(fs_info->fs_devices, 0);
	if (!io_stripe.dev) {
		ret = -EINVAL;
		goto out;
	}

	ret = btrfs_get_raid_extent_offset(fs_info, logical, &len, map_type, 0,
					   &io_stripe);
	if (ret)
		goto out;

	if (io_stripe.physical != logical) {
		test_err("invalid physical address, expected %llu, got %llu",
			 logical, io_stripe.physical);
		ret = -EINVAL;
		goto out;
	}

	if (len != SZ_64K) {
		test_err("invalid stripe length, expected %llu, got %llu",
			 (u64)SZ_64K, len);
		ret = -EINVAL;
		goto out;
	}

	for (int i = 0; i < RST_TEST_NUM_DEVICES; i++) {
		struct btrfs_io_stripe *stripe = &bioc->stripes[i];
		struct btrfs_device *dev;

		dev = btrfs_device_by_devid(fs_info->fs_devices, i);
		if (!dev) {
			ret = -EINVAL;
			goto out;
		}

		stripe->dev = dev;
		stripe->physical = SZ_1G + logical + i * SZ_1G;
	}

	ret = btrfs_insert_one_raid_extent(trans, bioc);
	if (ret)
		goto out;
	if (io_stripe.physical != logical + SZ_1G) {
		test_err("invalid physical address, expected %llu, got %llu",
			 logical + SZ_1G, io_stripe.physical);
		ret = -EINVAL;
		goto out;
	}

	if (len != SZ_64K) {
		test_err("invalid stripe length, expected %llu, got %llu",
			 (u64)SZ_64K, len);
		ret = -EINVAL;
		goto out;
	}

	ret = btrfs_delete_raid_extent(trans, logical, len);

out:
	btrfs_put_bioc(bioc);
	return ret;
}

static int test_simple_create_delete(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_io_context *bioc;
	struct btrfs_io_stripe io_stripe = { };
	u64 map_type = RST_TEST_RAID1_TYPE;
	u64 logical = SZ_1M;
	u64 len = SZ_64K;
	int ret;

	bioc = alloc_btrfs_io_context(fs_info, logical, RST_TEST_NUM_DEVICES);
	if (!bioc) {
		ret = -ENOMEM;
		goto out;
	}

	bioc->map_type = map_type;
	bioc->size = SZ_64K;

	for (int i = 0; i < RST_TEST_NUM_DEVICES; i++) {
		struct btrfs_io_stripe *stripe = &bioc->stripes[i];
		struct btrfs_device *dev;

		dev = btrfs_device_by_devid(fs_info->fs_devices, i);
		if (!dev) {
			ret = -EINVAL;
			goto out;
		}

		stripe->dev = dev;
		stripe->physical = logical + i * SZ_1G;
	}

	ret = btrfs_insert_one_raid_extent(trans, bioc);
	if (ret)
		goto out;

	io_stripe.dev = btrfs_device_by_devid(fs_info->fs_devices, 0);
	if (!io_stripe.dev) {
		ret = -EINVAL;
		goto out;
	}

	ret = btrfs_get_raid_extent_offset(fs_info, logical, &len, map_type, 0,
					   &io_stripe);
	if (ret)
		goto out;

	if (io_stripe.physical != logical) {
		test_err("invalid physical address, expected %llu, got %llu",
			 logical, io_stripe.physical);
		ret = -EINVAL;
		goto out;
	}

	if (len != SZ_64K) {
		test_err("invalid stripe length, expected %llu, got %llu",
			 (u64)SZ_64K, len);
		ret = -EINVAL;
		goto out;
	}

	ret = btrfs_delete_raid_extent(trans, logical, len);

out:
	btrfs_put_bioc(bioc);
	return ret;
}

test_func_t tests[] = {
	test_simple_create_delete,
	test_create_update_delete,
};

static int run_test(test_func_t test, u32 sectorsize, u32 nodesize)
{
	struct btrfs_trans_handle trans;
	struct btrfs_fs_info *fs_info;
	struct btrfs_root *root = NULL;
	int ret;

	fs_info = btrfs_alloc_dummy_fs_info(sectorsize, nodesize);
	if (!fs_info) {
		test_std_err(TEST_ALLOC_FS_INFO);
		ret = -ENOMEM;
		goto out;
	}

	root = btrfs_alloc_dummy_root(fs_info);
	if (IS_ERR(root)) {
		test_std_err(TEST_ALLOC_ROOT);
		ret = PTR_ERR(root);
		goto out;
	}
	btrfs_set_super_compat_ro_flags(root->fs_info->super_copy,
		BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE);
	root->root_key.objectid = BTRFS_RAID_STRIPE_TREE_OBJECTID;
	root->root_key.type = BTRFS_ROOT_ITEM_KEY;
	root->root_key.offset = 0;
	fs_info->stripe_root = root;
	root->fs_info->tree_root = root;

	root->node = alloc_test_extent_buffer(root->fs_info, nodesize);
	if (IS_ERR(root->node)) {
		test_std_err(TEST_ALLOC_EXTENT_BUFFER);
		ret = PTR_ERR(root->node);
		goto out;
	}
	btrfs_set_header_level(root->node, 0);
	btrfs_set_header_nritems(root->node, 0);
	root->alloc_bytenr += 2 * nodesize;

	for (int i = 0; i < RST_TEST_NUM_DEVICES; i++) {
		struct btrfs_device *dev;

		dev = btrfs_alloc_dummy_device(fs_info);
		dev->devid = i;
	}

	btrfs_init_dummy_trans(&trans, root->fs_info);
	ret = test(&trans);
	if (ret)
		goto out;

out:
	btrfs_free_dummy_root(root);
	btrfs_free_dummy_fs_info(fs_info);

	return ret;
}

int btrfs_test_raid_stripe_tree(u32 sectorsize, u32 nodesize)
{
	int ret = 0;

	test_msg("running RAID stripe-tree tests");
	for (int i = 0; i < ARRAY_SIZE(tests); i++) {
		ret = run_test(tests[i], sectorsize, nodesize);
		if (ret)
			goto out;
	}

out:
	return ret;
}
