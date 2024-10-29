// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Facebook */

#include "vmlinux.h"
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define FS_CREATE		0x00000100	/* Subfile was created */
#define FS_ISDIR		0x40000000	/* event occurred against dir */

struct {
	__uint(type, BPF_MAP_TYPE_INODE_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, __u32);
} inode_storage_map SEC(".maps");

int added_inode_storage;

SEC("struct_ops")
int BPF_PROG(bpf_fp_handler,
	     struct fsnotify_group *group,
	     struct fanotify_fastpath_hook *fp_hook,
	     struct fanotify_fastpath_event *fp_event)
{
	struct inode *dir;
	__u32 *value;

	dir = fp_event->dir;

	value = bpf_inode_storage_get(&inode_storage_map, dir, 0, 0);

	/* if dir doesn't have the tag, skip the event */
	if (!value)
		return FAN_FP_RET_SKIP_EVENT;

	/* propagate tag to subdir on fsnotify_mkdir */
	if (fp_event->mask == (FS_CREATE | FS_ISDIR) &&
	    fp_event->data_type == FSNOTIFY_EVENT_DENTRY) {
		struct inode *new_inode;

		new_inode = bpf_fanotify_data_inode(fp_event);
		if (!new_inode)
			goto out;

		value = bpf_inode_storage_get(&inode_storage_map, new_inode, 0,
					      BPF_LOCAL_STORAGE_GET_F_CREATE);
		if (value) {
			*value = 1;
			added_inode_storage++;
		}
		bpf_iput(new_inode);
	}
out:
	return FAN_FP_RET_SEND_TO_USERSPACE;
}

SEC("struct_ops")
int BPF_PROG(bpf_fp_init, struct fanotify_fastpath_hook *hook, const char *args)
{
	return 0;
}

SEC("struct_ops")
void BPF_PROG(bpf_fp_free, struct fanotify_fastpath_hook *hook)
{
}

SEC(".struct_ops.link")
struct fanotify_fastpath_ops bpf_fanotify_fastpath_ops = {
	.fp_handler = (void *)bpf_fp_handler,
	.fp_init = (void *)bpf_fp_init,
	.fp_free = (void *)bpf_fp_free,
	.name = "_tmp_test_sub_tree",
};

char _license[] SEC("license") = "GPL";
