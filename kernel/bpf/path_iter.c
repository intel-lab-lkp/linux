// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Meta Platforms, Inc. and affiliates. */
#include <linux/bpf.h>
#include <linux/bpf_mem_alloc.h>
#include <linux/namei.h>
#include <linux/path.h>

/* open-coded iterator */
struct bpf_iter_path {
	__u64 __opaque[3];
} __aligned(8);

struct bpf_iter_path_kern {
	struct path path;
	__u64 flags;
} __aligned(8);

__bpf_kfunc_start_defs();

__bpf_kfunc int bpf_iter_path_new(struct bpf_iter_path *it,
				  struct path *start,
				  __u64 flags)
{
	struct bpf_iter_path_kern *kit = (void *)it;

	BUILD_BUG_ON(sizeof(*kit) > sizeof(*it));
	BUILD_BUG_ON(__alignof__(*kit) != __alignof__(*it));

	if (flags) {
		memset(&kit->path, 0, sizeof(struct path));
		return -EINVAL;
	}

	kit->path = *start;
	path_get(&kit->path);
	kit->flags = flags;

	return 0;
}

__bpf_kfunc struct path *bpf_iter_path_next(struct bpf_iter_path *it)
{
	struct bpf_iter_path_kern *kit = (void *)it;
	struct path root = {};

	if (!path_walk_parent(&kit->path, &root))
		return NULL;
	return &kit->path;
}

__bpf_kfunc void bpf_iter_path_destroy(struct bpf_iter_path *it)
{
	struct bpf_iter_path_kern *kit = (void *)it;

	path_put(&kit->path);
}

__bpf_kfunc_end_defs();
