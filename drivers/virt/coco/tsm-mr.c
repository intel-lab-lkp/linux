// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024-2025 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/tsm-mr.h>

struct tm_context {
	struct rw_semaphore rwsem;
	struct attribute_group agrp;
	const struct tsm_measurements *tm;
	bool in_sync;
	struct bin_attribute mrs[];
};

static ssize_t tm_digest_read(struct file *filp, struct kobject *kobj,
			      const struct bin_attribute *attr, char *buffer,
			      loff_t off, size_t count)
{
	struct tm_context *ctx;
	const struct tsm_measurement_register *mr;
	int rc;

	ctx = attr->private;
	rc = down_read_interruptible(&ctx->rwsem);
	if (rc)
		return rc;

	/*
	 * @ctx->in_sync indicates if any MRs have been written since the last
	 * ctx->refresh() call. When @ctx->in_sync is false, ctx->refresh() is
	 * necessary to sync the cached values of all live MRs (i.e., with
	 * %TSM_MR_F_LIVE set) with the underlying hardware.
	 */
	mr = &ctx->tm->mrs[attr - ctx->mrs];
	if ((mr->mr_flags & TSM_MR_F_LIVE) && !ctx->in_sync) {
		up_read(&ctx->rwsem);

		rc = down_write_killable(&ctx->rwsem);
		if (rc)
			return rc;

		if (!ctx->in_sync) {
			rc = ctx->tm->refresh(ctx->tm, mr);
			ctx->in_sync = !rc;
		}

		downgrade_write(&ctx->rwsem);
	}

	memcpy(buffer, mr->mr_value + off, count);

	up_read(&ctx->rwsem);
	return rc ?: count;
}

static ssize_t tm_digest_write(struct file *filp, struct kobject *kobj,
			       const struct bin_attribute *attr, char *buffer,
			       loff_t off, size_t count)
{
	struct tm_context *ctx;
	const struct tsm_measurement_register *mr;
	ssize_t rc;

	/* partial writes are not supported */
	if (off != 0 || count != attr->size)
		return -EINVAL;

	ctx = attr->private;
	mr = &ctx->tm->mrs[attr - ctx->mrs];

	rc = down_write_killable(&ctx->rwsem);
	if (rc)
		return rc;

	rc = ctx->tm->write(ctx->tm, mr, buffer);

	/* reset @ctx->in_sync to refresh LIVE MRs on next read */
	if (!rc)
		ctx->in_sync = false;

	up_write(&ctx->rwsem);
	return rc ?: count;
}

/**
 * tsm_mr_create_attribute_group() - creates an attribute group for measurement
 * registers
 * @tm: pointer to &struct tsm_measurements containing the MR definitions.
 *
 * This function creates attributes corresponding to the MR definitions
 * provided by @tm->mrs.
 *
 * The created attributes will reference @tm and its members. The caller must
 * not free @tm until after tsm_mr_free_attribute_group() is called.
 *
 * Context: Process context. May sleep due to memory allocation.
 *
 * Return:
 * * On success, the pointer to a an attribute group is returned; otherwise
 * * %-EINVAL - Invalid MR definitions.
 * * %-ENOMEM - Out of memory.
 */
const struct attribute_group *__must_check
tsm_mr_create_attribute_group(const struct tsm_measurements *tm)
{
	if (!tm->mrs)
		return ERR_PTR(-EINVAL);

	/* aggregated length of all MR names */
	size_t nlen = 0;

	for (size_t i = 0; i < tm->nr_mrs; ++i) {
		if ((tm->mrs[i].mr_flags & TSM_MR_F_LIVE) && !tm->refresh)
			return ERR_PTR(-EINVAL);

		if ((tm->mrs[i].mr_flags & TSM_MR_F_WRITABLE) && !tm->write)
			return ERR_PTR(-EINVAL);

		if (tm->mrs[i].mr_flags & TSM_MR_F_NOHASH)
			continue;

		if (WARN_ON(tm->mrs[i].mr_hash >= HASH_ALGO__LAST))
			return ERR_PTR(-EINVAL);

		/* MR sysfs attribute names have the form of MRNAME:HASH */
		nlen += strlen(tm->mrs[i].mr_name) + 1 +
			strlen(hash_algo_name[tm->mrs[i].mr_hash]) + 1;
	}

	/*
	 * @bas and the MR name strings are combined into a single allocation
	 * so that we don't have to free MR names one-by-one in
	 * tsm_mr_free_attribute_group()
	 */
	struct bin_attribute **bas __free(kfree) =
		kzalloc(sizeof(*bas) * (tm->nr_mrs + 1) + nlen, GFP_KERNEL);
	struct tm_context *ctx __free(kfree) =
		kzalloc(struct_size(ctx, mrs, tm->nr_mrs), GFP_KERNEL);
	char *name, *end;

	if (!ctx || !bas)
		return ERR_PTR(-ENOMEM);

	/* @bas is followed immediately by MR name strings */
	name = (char *)&bas[tm->nr_mrs + 1];
	end = name + nlen;

	for (size_t i = 0; i < tm->nr_mrs; ++i) {
		bas[i] = &ctx->mrs[i];
		sysfs_bin_attr_init(bas[i]);

		if (tm->mrs[i].mr_flags & TSM_MR_F_NOHASH)
			bas[i]->attr.name = tm->mrs[i].mr_name;
		else if (name < end) {
			bas[i]->attr.name = name;
			name += snprintf(name, end - name, "%s:%s",
					 tm->mrs[i].mr_name,
					 hash_algo_name[tm->mrs[i].mr_hash]);
			++name;
		} else
			return ERR_PTR(-EINVAL);

		/* check for duplicated MR definitions */
		for (size_t j = 0; j < i; ++j)
			if (!strcmp(bas[i]->attr.name, bas[j]->attr.name))
				return ERR_PTR(-EINVAL);

		if (tm->mrs[i].mr_flags & TSM_MR_F_READABLE) {
			bas[i]->attr.mode |= 0444;
			bas[i]->read_new = tm_digest_read;
		}

		if (tm->mrs[i].mr_flags & TSM_MR_F_WRITABLE) {
			bas[i]->attr.mode |= 0220;
			bas[i]->write_new = tm_digest_write;
		}

		bas[i]->size = tm->mrs[i].mr_size;
		bas[i]->private = ctx;
	}

	if (name != end)
		return ERR_PTR(-EINVAL);

	init_rwsem(&ctx->rwsem);
	ctx->agrp.name = tm->name;
	ctx->agrp.bin_attrs = no_free_ptr(bas);
	ctx->tm = tm;
	return &no_free_ptr(ctx)->agrp;
}
EXPORT_SYMBOL_GPL(tsm_mr_create_attribute_group);

/**
 * tsm_mr_free_attribute_group() - frees the attribute group returned by
 * tsm_mr_create_attribute_group()
 * @attr_grp: attribute group returned by tsm_mr_create_attribute_group()
 *
 * Context: Process context.
 */
void tsm_mr_free_attribute_group(const struct attribute_group *attr_grp)
{
	kfree(attr_grp->bin_attrs);
	kfree(container_of(attr_grp, struct tm_context, agrp));
}
EXPORT_SYMBOL_GPL(tsm_mr_free_attribute_group);
