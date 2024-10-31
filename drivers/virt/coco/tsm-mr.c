// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/tsm.h>
#include <linux/shmem_fs.h>
#include <linux/ctype.h>
#include <crypto/hash_info.h>
#include <crypto/hash.h>

int tsm_mr_init(void);
void tsm_mr_exit(void);

enum _mrdir_bin_attr_index {
	_MRDIR_BA_DIGEST,
	_MRDIR_BA__COUNT,
};

struct _mrdir {
	struct kobject kobj;
	struct bin_attribute battrs[_MRDIR_BA__COUNT];
};

struct _mr_provider {
	struct kset kset;
	struct rw_semaphore rwsem;
	struct bin_attribute *mrfiles;
	struct tsm_measurement *tmr;
	bool in_sync;
};

static inline const struct tsm_measurement_register *
_mrdir_mr(const struct _mrdir *mrd)
{
	return (struct tsm_measurement_register *)mrd->battrs[_MRDIR_BA_DIGEST]
		.private;
}

static inline struct _mr_provider *
_mr_to_provider(const struct tsm_measurement_register *mr, struct kobject *kobj)
{
	if (mr->mr_flags & TSM_MR_F_F)
		return container_of(kobj, struct _mr_provider, kset.kobj);
	else
		return container_of(kobj->kset, struct _mr_provider, kset);
}

static inline int _call_refresh(struct _mr_provider *pvd,
				const struct tsm_measurement_register *mr)
{
	int rc = pvd->tmr->refresh(pvd->tmr, mr);
	if (rc)
		pr_warn(KBUILD_MODNAME ": %s.extend(%s) failed %d\n",
			kobject_name(&pvd->kset.kobj), mr->mr_name, rc);
	return rc;
}

static inline int _call_extend(struct _mr_provider *pvd,
			       const struct tsm_measurement_register *mr,
			       const u8 *data)
{
	int rc = pvd->tmr->extend(pvd->tmr, mr, data);
	if (rc)
		pr_warn(KBUILD_MODNAME ": %s.extend(%s) failed %d\n",
			kobject_name(&pvd->kset.kobj), mr->mr_name, rc);
	return rc;
}

static ssize_t hash_algo_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *page)
{
	struct _mrdir *mrd;
	mrd = container_of(kobj, typeof(*mrd), kobj);
	return sysfs_emit(page, "%s", hash_algo_name[_mrdir_mr(mrd)->mr_hash]);
}

static ssize_t _mr_read(struct file *filp, struct kobject *kobj,
			struct bin_attribute *attr, char *page, loff_t off,
			size_t count)
{
	const struct tsm_measurement_register *mr;
	struct _mr_provider *pvd;
	int rc;

	if (off < 0 || off > attr->size)
		return -EINVAL;

	count = min(count, attr->size - (size_t)off);
	if (!count)
		return count;

	mr = (typeof(mr))attr->private;
	BUG_ON(mr->mr_size != attr->size);

	pvd = _mr_to_provider(mr, kobj);
	rc = down_read_interruptible(&pvd->rwsem);
	if (rc)
		return rc;

	if ((mr->mr_flags & TSM_MR_F_L) && !pvd->in_sync) {
		up_read(&pvd->rwsem);

		rc = down_write_killable(&pvd->rwsem);
		if (rc)
			return rc;

		if (!pvd->in_sync) {
			rc = _call_refresh(pvd, mr);
			pvd->in_sync = !rc;
		}

		downgrade_write(&pvd->rwsem);
	}

	if (!rc)
		memcpy(page, mr->mr_value + off, count);
	else
		pr_debug(KBUILD_MODNAME ": %s.refresh(%s)=%d\n",
			 kobject_name(&pvd->kset.kobj), mr->mr_name, rc);

	up_read(&pvd->rwsem);
	return rc ?: count;
}

static ssize_t _mr_write(struct file *filp, struct kobject *kobj,
			 struct bin_attribute *attr, char *page, loff_t off,
			 size_t count)
{
	const struct tsm_measurement_register *mr;
	struct _mr_provider *pvd;
	ssize_t rc;

	if (off != 0 || count != attr->size)
		return -EINVAL;

	mr = (typeof(mr))attr->private;
	BUG_ON(mr->mr_size != attr->size);

	pvd = _mr_to_provider(mr, kobj);
	rc = down_write_killable(&pvd->rwsem);
	if (rc)
		return rc;

	if (mr->mr_flags & TSM_MR_F_X)
		rc = _call_extend(pvd, mr, page);
	else
		memcpy(mr->mr_value, page, count);

	if (!rc)
		pvd->in_sync = false;

	up_write(&pvd->rwsem);
	return rc ?: count;
}

static void _mrdir_release(struct kobject *kobj)
{
	struct _mrdir *mrd;
	mrd = container_of(kobj, typeof(*mrd), kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	kfree(mrd);
}

static struct kobj_type _mrdir_ktype = {
	.release = _mrdir_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct _mrdir *_mrdir_create(const struct tsm_measurement_register *mr,
				    struct _mr_provider *pvd)
{
	struct _mrdir *mrd __free(kfree);
	int rc;

	BUG_ON(mr->mr_flags & TSM_MR_F_F);
	mrd = kzalloc(sizeof(*mrd), GFP_KERNEL);
	if (!mrd)
		return ERR_PTR(-ENOMEM);

	sysfs_bin_attr_init(&mrd->battrs[_MRDIR_BA_DIGEST]);
	mrd->battrs[_MRDIR_BA_DIGEST].attr.name = "digest";
	if (mr->mr_flags & TSM_MR_F_W)
		mrd->battrs[_MRDIR_BA_DIGEST].attr.mode |= S_IWUSR | S_IWGRP;
	if (mr->mr_flags & TSM_MR_F_R)
		mrd->battrs[_MRDIR_BA_DIGEST].attr.mode |= S_IRUGO;

	mrd->battrs[_MRDIR_BA_DIGEST].size = mr->mr_size;
	mrd->battrs[_MRDIR_BA_DIGEST].read = _mr_read;
	mrd->battrs[_MRDIR_BA_DIGEST].write = _mr_write;
	mrd->battrs[_MRDIR_BA_DIGEST].private = (void *)mr;

	mrd->kobj.kset = &pvd->kset;
	rc = kobject_init_and_add(&mrd->kobj, &_mrdir_ktype, NULL, "%s",
				  mr->mr_name);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(mrd);
}

static void _mr_provider_release(struct kobject *kobj)
{
	struct _mr_provider *pvd;
	pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	BUG_ON(!list_empty(&pvd->kset.list));
	kfree(pvd->mrfiles);
	kfree(pvd);
}

static struct kobj_type _mr_provider_ktype = {
	.release = _mr_provider_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct kset *_sysfs_tsm;

static struct _mr_provider *_mr_provider_create(struct tsm_measurement *tmr)
{
	struct _mr_provider *pvd __free(kfree);
	int rc;

	pvd = kzalloc(sizeof(*pvd), GFP_KERNEL);
	if (!pvd)
		return ERR_PTR(-ENOMEM);

	if (!tmr->name || !tmr->mrs || !tmr->refresh || !tmr->extend)
		return ERR_PTR(-EINVAL);

	rc = kobject_set_name(&pvd->kset.kobj, "%s", tmr->name);
	if (rc)
		return ERR_PTR(rc);

	pvd->kset.kobj.kset = _sysfs_tsm;
	pvd->kset.kobj.ktype = &_mr_provider_ktype;
	pvd->tmr = tmr;

	init_rwsem(&pvd->rwsem);

	rc = kset_register(&pvd->kset);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(pvd);
}

DEFINE_FREE(_unregister_measurement, struct _mr_provider *,
	    if (!IS_ERR_OR_NULL(_T)) tsm_unregister_measurement(_T->tmr));

int tsm_register_measurement(struct tsm_measurement *tmr)
{
	static struct kobj_attribute _attr_hash = __ATTR_RO(hash_algo);

	struct _mr_provider *pvd __free(_unregister_measurement);
	int rc, nr;

	pvd = _mr_provider_create(tmr);
	if (IS_ERR(pvd))
		return PTR_ERR(pvd);

	nr = 0;
	for (int i = 0; tmr->mrs[i].mr_name; ++i) {
		// flat files are counted and skipped
		if (tmr->mrs[i].mr_flags & TSM_MR_F_F) {
			++nr;
			continue;
		}

		struct _mrdir *mrd = _mrdir_create(&tmr->mrs[i], pvd);
		if (IS_ERR(mrd))
			return PTR_ERR(mrd);

		struct attribute *attrs[] = {
			&_attr_hash.attr,
			NULL,
		};
		struct bin_attribute *battrs[_MRDIR_BA__COUNT + 1] = {};
		for (int j = 0; j < _MRDIR_BA__COUNT; ++j)
			battrs[j] = &mrd->battrs[j];
		struct attribute_group agrp = {
			.attrs = attrs,
			.bin_attrs = battrs,
		};
		rc = sysfs_create_group(&mrd->kobj, &agrp);
		if (rc)
			return rc;
	}

	if (nr > 0) {
		struct bin_attribute *mrfiles __free(kfree);
		struct bin_attribute **battrs __free(kfree);

		mrfiles = kcalloc(sizeof(*mrfiles), nr, GFP_KERNEL);
		battrs = kcalloc(sizeof(*battrs), nr + 1, GFP_KERNEL);
		if (!battrs || !mrfiles)
			return -ENOMEM;

		for (int i = 0, j = 0; tmr->mrs[i].mr_name; ++i) {
			if (!(tmr->mrs[i].mr_flags & TSM_MR_F_F))
				continue;

			mrfiles[j].attr.name = tmr->mrs[i].mr_name;
			mrfiles[j].read = _mr_read;
			mrfiles[j].write = _mr_write;
			mrfiles[j].size = tmr->mrs[i].mr_size;
			mrfiles[j].private = (void *)&tmr->mrs[i];
			if (tmr->mrs[i].mr_flags & TSM_MR_F_R)
				mrfiles[j].attr.mode |= S_IRUGO;
			if (tmr->mrs[i].mr_flags & TSM_MR_F_W)
				mrfiles[j].attr.mode |= S_IWUSR | S_IWGRP;

			battrs[j] = &mrfiles[j];
			++j;

			BUG_ON(j > nr);
		}

		struct attribute_group agrp = {
			.bin_attrs = battrs,
		};
		rc = sysfs_create_group(&pvd->kset.kobj, &agrp);
		if (rc)
			return rc;

		pvd->mrfiles = no_free_ptr(mrfiles);
	}

	pvd = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_register_measurement);

static void _kset_put_children(struct kset *kset)
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
	struct kobject *kobj = kset_find_obj(_sysfs_tsm, tmr->name);
	if (!kobj)
		return -ENOENT;

	struct _mr_provider *pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	BUG_ON(pvd->tmr != tmr);

	_kset_put_children(&pvd->kset);
	kset_unregister(&pvd->kset);
	kobject_put(kobj);
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_unregister_measurement);

int tsm_mr_init(void)
{
	_sysfs_tsm = kset_create_and_add("tsm", NULL, kernel_kobj);
	if (!_sysfs_tsm)
		return -ENOMEM;
	return 0;
}

void tsm_mr_exit(void)
{
	kset_unregister(_sysfs_tsm);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Provide Trusted Security Module measurements via sysfs");
