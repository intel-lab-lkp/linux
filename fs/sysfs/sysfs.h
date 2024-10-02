/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs/sysfs/sysfs.h - sysfs internal header file
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 */

#ifndef __SYSFS_INTERNAL_H
#define __SYSFS_INTERNAL_H

#include <linux/sysfs.h>

/*
 * mount.c
 */
extern struct kernfs_node *sysfs_root_kn;

/*
 * dir.c
 */
extern spinlock_t sysfs_symlink_target_lock;

void sysfs_warn_dup(struct kernfs_node *parent, const char *name);

/*
 * file.c
 */
int sysfs_add_file_mode_ns(struct kernfs_node *parent,
		const struct attribute *attr, umode_t amode, kuid_t uid,
		kgid_t gid, const void *ns);
int sysfs_add_bin_file_mode_ns(struct kernfs_node *parent,
		const struct bin_attribute *battr, umode_t mode,
		kuid_t uid, kgid_t gid, const void *ns);

/*
 * symlink.c
 */
int sysfs_create_link_sd(struct kernfs_node *kn, struct kobject *target,
			 const char *name);

#define SYSFS_LOCKED		040000

/*
 * uevent_show() needs this due to userspace expectations of reading
 * that attribute flushing device probing, it is not intended to be a
 * general semantic for other device sysfs attributes. It is broken to
 * use this with non-device / pure-kobject sysfs attributes, see
 * sysfs_kf_setup_lock().
 */
#define __ATTR_LOCKED(_name, _mode, _show, _store) {			\
	.attr = {.name = __stringify(_name),				\
		 .mode = SYSFS_LOCKED | VERIFY_OCTAL_PERMISSIONS(_mode) },\
	.show	= _show,						\
	.store	= _store,						\
}

#define __ATTR_LOCKED_RW(_name) __ATTR_LOCKED(_name, 0644, _name##_show, _name##_store)

#endif	/* __SYSFS_INTERNAL_H */
