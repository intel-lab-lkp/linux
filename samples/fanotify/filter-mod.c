// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#include <linux/fsnotify.h>
#include <linux/fanotify.h>
#include <linux/module.h>
#include <linux/path.h>
#include <linux/file.h>
#include "filter.h"

struct fan_filter_sample_data {
	struct path subtree_path;
	enum fan_filter_sample_mode mode;
};

static int sample_filter(struct fsnotify_group *group,
			 struct fanotify_filter_hook *filter_hook,
			 struct fanotify_filter_event *filter_event)
{
	struct fan_filter_sample_data *data;
	struct dentry *dentry;

	dentry = fsnotify_data_dentry(filter_event->data, filter_event->data_type);
	if (!dentry)
		return FAN_FILTER_RET_SEND_TO_USERSPACE;

	data = filter_hook->data;

	if (is_subdir(dentry, data->subtree_path.dentry)) {
		if (data->mode == FAN_FILTER_SAMPLE_MODE_BLOCK)
			return -EPERM;
		return FAN_FILTER_RET_SEND_TO_USERSPACE;
	}
	return FAN_FILTER_RET_SKIP_EVENT;
}

static int sample_filter_init(struct fsnotify_group *group,
			      struct fanotify_filter_hook *filter_hook,
			      void *argp)
{
	struct fan_filter_sample_args *args;
	struct fan_filter_sample_data *data;
	struct file *file;
	int fd;

	args = (struct fan_filter_sample_args *)argp;
	fd = args->subtree_fd;

	file = fget(fd);
	if (!file)
		return -EBADF;
	data = kzalloc(sizeof(struct fan_filter_sample_data), GFP_KERNEL);
	if (!data) {
		fput(file);
		return -ENOMEM;
	}
	path_get(&file->f_path);
	data->subtree_path = file->f_path;
	fput(file);
	data->mode = args->mode;
	filter_hook->data = data;
	return 0;
}

static void sample_filter_free(struct fanotify_filter_hook *filter_hook)
{
	struct fan_filter_sample_data *data = filter_hook->data;

	path_put(&data->subtree_path);
	kfree(data);
}

static struct fanotify_filter_ops fan_filter_sample_ops = {
	.filter = sample_filter,
	.filter_init = sample_filter_init,
	.filter_free = sample_filter_free,
	.name = "monitor-subtree",
	.owner = THIS_MODULE,
	.flags = FAN_FILTER_F_SYS_ADMIN_ONLY,
	.init_args_size = sizeof(struct fan_filter_sample_args),
	.desc =
	"mode = 1: only emit events under a subtree\n"
	"mode = 2: block accesses under a subtree",
	.init_args =
	"struct fan_filter_sample_args {\n"
	"    int subtree_fd;\n"
	"    enum fan_filter_sample_mode mode;\n"
	"};",
};

static int __init fanotify_filter_sample_init(void)
{
	return fanotify_filter_register(&fan_filter_sample_ops);
}
static void __exit fanotify_filter_sample_exit(void)
{
	fanotify_filter_unregister(&fan_filter_sample_ops);
}

module_init(fanotify_filter_sample_init);
module_exit(fanotify_filter_sample_exit);

MODULE_AUTHOR("Song Liu");
MODULE_DESCRIPTION("Example fanotify filter handler");
MODULE_LICENSE("GPL");
