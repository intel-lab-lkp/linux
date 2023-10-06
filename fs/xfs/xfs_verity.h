// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Red Hat, Inc.
 */
#ifndef __XFS_VERITY_H__
#define __XFS_VERITY_H__

#include "xfs.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include <linux/fsverity.h>

#define XFS_VERITY_DESCRIPTOR_NAME "verity_descriptor"
#define XFS_VERITY_DESCRIPTOR_NAME_LEN 17

static inline bool
xfs_verity_merkle_block(
		struct xfs_da_args *args)
{
	if (!(args->attr_filter & XFS_ATTR_VERITY))
		return false;

	if (!(args->op_flags & XFS_DA_OP_BUFFER))
		return false;

	if (args->valuelen < 1024 || args->valuelen > PAGE_SIZE ||
			!is_power_of_2(args->valuelen))
		return false;

	return true;
}

#ifdef CONFIG_FS_VERITY
extern const struct fsverity_operations xfs_verity_ops;
#endif	/* CONFIG_FS_VERITY */

#endif	/* __XFS_VERITY_H__ */
