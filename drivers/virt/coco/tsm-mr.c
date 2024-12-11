// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/hash.h>
#include <crypto/hash_info.h>
#include <linux/ctype.h>
#include <linux/shmem_fs.h>
#include <linux/tsm.h>

int tsm_mr_init(void);
void tsm_mr_exit(void);

enum tmr_dir_battr_index {
	TMR_DIR_BA_DIGEST,
	TMR_DIR_BA__COUNT,

	TMR_DIR__ALGO_MAX = 4,
};

struct tmr_dir {
	struct kobject kobj;
	struct bin_attribute battrs[TMR_DIR__ALGO_MAX][TMR_DIR_BA__COUNT];
	int algo;
};

struct tmr_provider {
	struct kset kset;
	struct rw_semaphore rwsem;
	struct bin_attribute *mrfiles;
	struct tsm_measurement *tmr;
	bool in_sync;
};

static inline struct tmr_provider *tmr_mr_to_provider(const struct tsm_measurement_register *mr,
						      struct kobject *kobj)
{
	if (mr->mr_flags & TSM_MR_F_F)
		return container_of(kobj, struct tmr_provider, kset.kobj);
	else
		return container_of(kobj->kset, struct tmr_provider, kset);
}

static inline int tmr_call_refresh(struct tmr_provider *pvd,
				   const struct tsm_measurement_register *mr)
{
	int rc;

	rc = pvd->tmr->refresh(pvd->tmr, mr);
	if (rc)
		pr_warn("%s.extend(%s) failed %d\n", kobject_name(&pvd->kset.kobj), mr->mr_name,
			rc);
	return rc;
}

static inline int tmr_call_extend(struct tmr_provider *pvd,
				  const struct tsm_measurement_register *mr, const u8 *data)
{
	int rc;

	rc = pvd->tmr->extend(pvd->tmr, mr, data);
	if (rc)
		pr_warn("%s.extend(%s) failed %d\n", kobject_name(&pvd->kset.kobj), mr->mr_name,
			rc);
	return rc;
}

static ssize_t tmr_digest_read(struct file *filp, struct kobject *kobj, struct bin_attribute *attr,
			       char *page, loff_t off, size_t count)
{
	const struct tsm_measurement_register *mr;
	struct tmr_provider *pvd;
	int rc;

	if (off < 0 || off > attr->size)
		return -EINVAL;

	count = min(count, attr->size - (size_t)off);
	if (!count)
		return count;

	mr = (typeof(mr))attr->private;
	pvd = tmr_mr_to_provider(mr, kobj);
	rc = down_read_interruptible(&pvd->rwsem);
	if (rc)
		return rc;

	if ((mr->mr_flags & TSM_MR_F_L) && !pvd->in_sync) {
		up_read(&pvd->rwsem);

		rc = down_write_killable(&pvd->rwsem);
		if (rc)
			return rc;

		if (!pvd->in_sync) {
			rc = tmr_call_refresh(pvd, mr);
			pvd->in_sync = !rc;
		}

		downgrade_write(&pvd->rwsem);
	}

	if (!rc)
		memcpy(page, mr->mr_value + off, count);
	else
		pr_debug("%s.refresh(%s)=%d\n", kobject_name(&pvd->kset.kobj), mr->mr_name, rc);

	up_read(&pvd->rwsem);
	return rc ?: count;
}

static ssize_t tmr_digest_write(struct file *filp, struct kobject *kobj, struct bin_attribute *attr,
				char *page, loff_t off, size_t count)
{
	const struct tsm_measurement_register *mr;
	struct tmr_provider *pvd;
	ssize_t rc;

	if (off != 0 || count != attr->size)
		return -EINVAL;

	mr = (typeof(mr))attr->private;
	pvd = tmr_mr_to_provider(mr, kobj);
	rc = down_write_killable(&pvd->rwsem);
	if (rc)
		return rc;

	if (mr->mr_flags & TSM_MR_F_X)
		rc = tmr_call_extend(pvd, mr, page);
	else
		memcpy(mr->mr_value, page, count);

	if (!rc)
		pvd->in_sync = false;

	up_write(&pvd->rwsem);
	return rc ?: count;
}

static void tmr_dir_release(struct kobject *kobj)
{
	struct tmr_dir *mrd;

	mrd = container_of(kobj, typeof(*mrd), kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	kfree(mrd);
}

static const struct kobj_type tmr_dir_ktype = {
	.release = tmr_dir_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct tmr_dir *tmr_dir_create(const struct tsm_measurement_register *mr,
				      struct tmr_provider *pvd)
{
	struct kobject *kobj;
	struct tmr_dir *mrd;

	kobj = kset_find_obj(&pvd->kset, mr->mr_name);
	if (kobj) {
		mrd = container_of(kobj, typeof(*mrd), kobj);
		kobject_put(kobj);
		if (++mrd->algo >= TMR_DIR__ALGO_MAX) {
			--mrd->algo;
			return ERR_PTR(-ENOSPC);
		}
	} else {
		int rc;

		mrd = kzalloc(sizeof(*mrd), GFP_KERNEL);
		if (!mrd)
			return ERR_PTR(-ENOMEM);

		mrd->kobj.kset = &pvd->kset;
		rc = kobject_init_and_add(&mrd->kobj, &tmr_dir_ktype, NULL, "%s", mr->mr_name);
		if (rc) {
			kfree(mrd);
			return ERR_PTR(rc);
		}
	}

	sysfs_bin_attr_init(&mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST]);
	mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].attr.name = "digest";
	if (mr->mr_flags & TSM_MR_F_W)
		mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].attr.mode |= S_IWUSR | S_IWGRP;
	if (mr->mr_flags & TSM_MR_F_R)
		mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].attr.mode |= S_IRUGO;

	mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].size = mr->mr_size;
	mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].read = tmr_digest_read;
	mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].write = tmr_digest_write;
	mrd->battrs[mrd->algo][TMR_DIR_BA_DIGEST].private = (void *)mr;

	return mrd;
}

static void tmr_provider_release(struct kobject *kobj)
{
	struct tmr_provider *pvd;

	pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	if (!WARN_ON(!list_empty(&pvd->kset.list))) {
		kfree(pvd->mrfiles);
		kfree(pvd);
	}
}

static const struct kobj_type _mr_provider_ktype = {
	.release = tmr_provider_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct kset *tmr_sysfs_root;

static struct tmr_provider *tmr_provider_create(struct tsm_measurement *tmr)
{
	struct tmr_provider *pvd __free(kfree);
	int rc;

	pvd = kzalloc(sizeof(*pvd), GFP_KERNEL);
	if (!pvd)
		return ERR_PTR(-ENOMEM);

	if (!tmr->name || !tmr->mrs || !tmr->refresh || !tmr->extend)
		return ERR_PTR(-EINVAL);

	rc = kobject_set_name(&pvd->kset.kobj, "%s", tmr->name);
	if (rc)
		return ERR_PTR(rc);

	pvd->kset.kobj.kset = tmr_sysfs_root;
	pvd->kset.kobj.ktype = &_mr_provider_ktype;
	pvd->tmr = tmr;

	init_rwsem(&pvd->rwsem);

	rc = kset_register(&pvd->kset);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(pvd);
}

DEFINE_FREE(_unregister_measurement, struct tmr_provider *,
	    if (!IS_ERR_OR_NULL(_T)) tsm_unregister_measurement(_T->tmr));

int tsm_register_measurement(struct tsm_measurement *tmr)
{
	struct tmr_provider *pvd __free(_unregister_measurement);
	int rc, nr;

	pvd = tmr_provider_create(tmr);
	if (IS_ERR(pvd))
		return PTR_ERR(pvd);

	nr = 0;
	for (int i = 0; tmr->mrs[i].mr_name; ++i) {
		// flat files are counted and skipped
		if (tmr->mrs[i].mr_flags & TSM_MR_F_F) {
			++nr;
			continue;
		}

		struct tmr_dir *mrd;
		struct bin_attribute *battrs[TMR_DIR_BA__COUNT + 1] = {};
		struct attribute_group agrp = {
			.name = hash_algo_name[tmr->mrs[i].mr_hash],
			.bin_attrs = battrs,
		};

		mrd = tmr_dir_create(&tmr->mrs[i], pvd);
		if (IS_ERR(mrd))
			return PTR_ERR(mrd);

		for (int j = 0; j < TMR_DIR_BA__COUNT; ++j)
			battrs[j] = &mrd->battrs[mrd->algo][j];

		rc = sysfs_create_group(&mrd->kobj, &agrp);
		if (rc)
			return rc;
	}

	if (nr > 0) {
		struct bin_attribute *mrfiles __free(kfree);
		struct bin_attribute **battrs __free(kfree);

		mrfiles = kcalloc(nr, sizeof(*mrfiles), GFP_KERNEL);
		battrs = kcalloc(nr + 1, sizeof(*battrs), GFP_KERNEL);
		if (!battrs || !mrfiles)
			return -ENOMEM;

		for (int i = 0, j = 0; tmr->mrs[i].mr_name; ++i) {
			if (!(tmr->mrs[i].mr_flags & TSM_MR_F_F))
				continue;

			mrfiles[j].attr.name = tmr->mrs[i].mr_name;
			mrfiles[j].read = tmr_digest_read;
			mrfiles[j].write = tmr_digest_write;
			mrfiles[j].size = tmr->mrs[i].mr_size;
			mrfiles[j].private = (void *)&tmr->mrs[i];
			if (tmr->mrs[i].mr_flags & TSM_MR_F_R)
				mrfiles[j].attr.mode |= S_IRUGO;
			if (tmr->mrs[i].mr_flags & TSM_MR_F_W)
				mrfiles[j].attr.mode |= S_IWUSR | S_IWGRP;

			battrs[j] = &mrfiles[j];
			++j;
		}

		struct attribute_group agrp = {
			.bin_attrs = battrs,
		};
		rc = sysfs_create_group(&pvd->kset.kobj, &agrp);
		if (rc)
			return rc;

		pvd->mrfiles = no_free_ptr(mrfiles);
	}

	// set pvd to NULL or it will be freed due to __free(kfree)
	pvd = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_register_measurement);

static void tmr_put_children(struct kset *kset)
{
	struct kobject *p, *n;

	spin_lock(&kset->list_lock);
	list_for_each_entry_safe(p, n, &kset->list, entry) {
		spin_unlock(&kset->list_lock);
		kobject_put(p);
		spin_lock(&kset->list_lock);
	}
	spin_unlock(&kset->list_lock);
}

int tsm_unregister_measurement(struct tsm_measurement *tmr)
{
	struct kobject *kobj;
	struct tmr_provider *pvd;

	kobj = kset_find_obj(tmr_sysfs_root, tmr->name);
	if (!kobj)
		return -ENOENT;

	pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	if (pvd->tmr != tmr)
		return -EINVAL;

	tmr_put_children(&pvd->kset);
	kset_unregister(&pvd->kset);
	kobject_put(kobj);
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_unregister_measurement);

int tsm_mr_init(void)
{
	tmr_sysfs_root = kset_create_and_add("tsm", NULL, kernel_kobj);
	if (!tmr_sysfs_root)
		return -ENOMEM;
	return 0;
}

void tsm_mr_exit(void)
{
	kset_unregister(tmr_sysfs_root);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Provide Trusted Security Module measurements via sysfs");
