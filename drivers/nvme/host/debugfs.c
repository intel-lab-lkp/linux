// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 IBM Corporation
 *	Nilay Shroff <nilay@linux.ibm.com>
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "nvme.h"

struct nvme_debugfs_attr {
	const char *name;
	umode_t mode;
	int (*show)(void *data, struct seq_file *m);
	const struct seq_operations *seq_ops;
	bool (*get)(void *data);
	void (*put)(void *data);
};

struct nvme_debugfs_ctx {
	void *data;
	struct nvme_debugfs_attr *attr;
};

static int nvme_debugfs_show(struct seq_file *m, void *v)
{
	struct nvme_debugfs_ctx *ctx = m->private;
	void *data = ctx->data;
	struct nvme_debugfs_attr *attr = ctx->attr;

	return attr->show(data, m);
}

static int nvme_debugfs_open(struct inode *inode, struct file *file)
{
	void *data = inode->i_private;
	struct nvme_debugfs_attr *attr = debugfs_get_aux(file);
	struct nvme_debugfs_ctx *ctx;
	struct seq_file *m;
	int ret;

	if (attr->get && !attr->get(data))
		return -ENODEV;

	ctx = kzalloc_obj(*ctx);
	if (WARN_ON_ONCE(!ctx)) {
		ret = -ENOMEM;
		goto out;
	}

	ctx->data = data;
	ctx->attr = attr;

	if (attr->seq_ops) {
		ret = seq_open(file, attr->seq_ops);
		if (ret)
			goto out_free;

		m = file->private_data;
		m->private = ctx;
		return ret;
	}

	if (WARN_ON_ONCE(!attr->show)) {
		ret = -EPERM;
		goto out_free;
	}

	ret = single_open(file, nvme_debugfs_show, ctx);
	if (!ret)
		return ret;

out_free:
	kfree(ctx);
out:
	if (attr->put)
		attr->put(data);
	return ret;
}

static int nvme_debugfs_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct nvme_debugfs_ctx *ctx = m->private;
	struct nvme_debugfs_attr *attr = ctx->attr;
	int ret;

	if (attr->seq_ops)
		ret = seq_release(inode, file);
	else
		ret = single_release(inode, file);

	if (attr->put)
		attr->put(ctx->data);

	kfree(ctx);
	return ret;
}

static const struct file_operations nvme_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = nvme_debugfs_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = nvme_debugfs_release,
};

static const struct nvme_debugfs_attr nvme_ns_debugfs_attrs[] = {
	{},
};

static void nvme_debugfs_create_files(struct request_queue *q,
		const struct nvme_debugfs_attr *attr, void *data)
{
	if (WARN_ON_ONCE(!q->debugfs_dir))
		return;

	for (; attr->name; attr++)
		debugfs_create_file_aux(attr->name, attr->mode, q->debugfs_dir,
				data, (void *)attr, &nvme_debugfs_fops);
}

void nvme_debugfs_register(struct gendisk *disk)
{
	nvme_debugfs_create_files(disk->queue, nvme_ns_debugfs_attrs,
			disk->private_data);
}
