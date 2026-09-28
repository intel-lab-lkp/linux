// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sysfs interface for overlayfs.
 */
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/unicode.h>
#include <linux/xattr.h>
#include "overlayfs.h"

static struct kset *ovl_kset;

struct ovl_sysfs_attr {
	struct attribute attr;
	ssize_t (*show)(struct ovl_fs *ofs, char *buf);
};

#define OVL_SYSFS_ATTR_RO(_name)					\
	static struct ovl_sysfs_attr ovl_sysfs_attr_##_name = {		\
		.attr	= { .name = __stringify(_name), .mode = 0444 },	\
		.show	= ovl_##_name##_show,				\
	}

static struct attribute *ovl_sysfs_attrs[] = {
	NULL,
};
ATTRIBUTE_GROUPS(ovl_sysfs);

static ssize_t ovl_sysfs_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct ovl_fs *ofs = container_of(kobj, struct ovl_fs, kobj);
	struct ovl_sysfs_attr *a = container_of(attr, struct ovl_sysfs_attr,
						attr);

	return a->show(ofs, buf);
}

static const struct sysfs_ops ovl_sysfs_ops = {
	.show	= ovl_sysfs_show,
};

static void ovl_sysfs_release(struct kobject *kobj)
{
	struct ovl_fs *ofs = container_of(kobj, struct ovl_fs, kobj);

	complete(&ofs->kobj_unregister);
}

static const struct kobj_type ovl_sysfs_ktype = {
	.default_groups	= ovl_sysfs_groups,
	.sysfs_ops	= &ovl_sysfs_ops,
	.release	= ovl_sysfs_release,
};

int ovl_sysfs_register(struct ovl_fs *ofs)
{
	struct super_block *sb = ofs->sb;
	int err;

	init_completion(&ofs->kobj_unregister);
	ofs->kobj.kset = ovl_kset;
	err = kobject_init_and_add(&ofs->kobj, &ovl_sysfs_ktype, NULL, "%u:%u",
				   MAJOR(sb->s_dev), MINOR(sb->s_dev));
	if (err) {
		kobject_put(&ofs->kobj);
		wait_for_completion(&ofs->kobj_unregister);
		pr_warn("failed to create sysfs volume %u:%u (err=%d).\n",
			MAJOR(sb->s_dev), MINOR(sb->s_dev), err);
		return err;
	}

	ofs->kobj_registered = true;
	kobject_uevent(&ofs->kobj, KOBJ_ADD);
	return 0;
}

void ovl_sysfs_unregister(struct ovl_fs *ofs)
{
	if (!ofs->kobj_registered)
		return;

	ofs->kobj_registered = false;
	kobject_del(&ofs->kobj);
	kobject_put(&ofs->kobj);
	wait_for_completion(&ofs->kobj_unregister);
}

int __init ovl_sysfs_init(void)
{
	ovl_kset = kset_create_and_add("overlay", NULL, fs_kobj);
	if (!ovl_kset)
		return -ENOMEM;
	return 0;
}

void ovl_sysfs_exit(void)
{
	kset_unregister(ovl_kset);
}
