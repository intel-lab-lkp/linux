// SPDX-License-Identifier: GPL-2.0-or-later
/* core.c - Live Dump's main
 * Copyright (C) 2012 Hitachi, Ltd.
 * Copyright (C) 2023 SUSE
 * Author: YOSHIDA Masanori <masanori.yoshida.tv@hitachi.com>
 * Author: Lukas Hruska <lhruska@suse.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "memdump.h"
#include <asm/wrprotect.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/sysfs.h>
#include <linux/memblock.h>

#define DEVICE_NAME	"livedump"

enum state {
	LIVEDUMP_STATE_UNDEFINED,
	LIVEDUMP_STATE_INIT,
	LIVEDUMP_STATE_START,
	LIVEDUMP_STATE_SWEEP,
	LIVEDUMP_STATE_FINISH,
	LIVEDUMP_STATE_UNINIT,
};

struct livedump_conf {
	char bdevpath[PATH_MAX];
} livedump_conf;

enum state livedump_state;

static void do_uninit(void)
{
	wrprotect_uninit();
	livedump_memdump_uninit();
}

static int do_init(void)
{
	int ret;

	if (strlen(livedump_conf.bdevpath) == 0) {
		ret = -EINVAL;
		goto err;
	}

	ret = wrprotect_init(livedump_memdump_handle_page, livedump_memdump_sm_init);
	if (ret) {
		pr_warn("livedump: Failed to initialize Protection manager.\n");
		goto err;
	}

	ret = livedump_memdump_init(livedump_conf.bdevpath);
	if (ret) {
		pr_warn("livedump: Failed to initialize Dump manager.\n");
		goto err;
	}

	return 0;
err:
	do_uninit();
	return ret;
}

static long livedump_change_state(unsigned int cmd)
{
	long ret = 0;

	if (cmd == LIVEDUMP_STATE_UNDEFINED) {
		pr_warn("livedump: you cannot change the livedump state into LIVEDUMP_STATE_UNDEFINED.\n");
		return -EINVAL;
	}

	/* All states except LIVEDUMP_STATE_UNINIT must have an output set. */
	switch (cmd) {
	case LIVEDUMP_STATE_UNINIT:
		break;
	default:
		if (!strlen(livedump_conf.bdevpath)) {
			pr_warn("livedump: The output must be set first before changing the state.\n");
			return -EINVAL;
		}
	}

	switch (cmd) {
	case LIVEDUMP_STATE_INIT:
		if (livedump_state != LIVEDUMP_STATE_UNDEFINED &&
		    livedump_state != LIVEDUMP_STATE_UNINIT) {
			pr_warn("livedump: To initialize a livedump the current state must be "
			    "LIVEDUMP_STATE_UNDEFINED or LIVEDUMP_STATE_UNINIT.\n");
			return -EINVAL;
		}
		ret = do_init();
		break;
	case LIVEDUMP_STATE_START:
		if (livedump_state != LIVEDUMP_STATE_INIT) {
			pr_warn("livedump: To start a livedump the current state must be "
			    "LIVEDUMP_STATE_INIT.\n");
			return -EINVAL;
		}
		ret = wrprotect_start();
		break;
	case LIVEDUMP_STATE_SWEEP:
		if (livedump_state != LIVEDUMP_STATE_START) {
			pr_warn("livedump: To start sweep functionality of livedump the current state must "
			    "be LIVEDUMP_STATE_START.\n");
			return -EINVAL;
		}
		ret = wrprotect_sweep();
		break;
	case LIVEDUMP_STATE_FINISH:
		if (livedump_state != LIVEDUMP_STATE_SWEEP) {
			pr_warn("livedump: To finish a livedump the current state must be "
			    "LIVEDUMP_STATE_SWEEP.\n");
			return -EINVAL;
		}
		livedump_memdump_write_elf_hdr();
		break;
	case LIVEDUMP_STATE_UNINIT:
		if (livedump_state < LIVEDUMP_STATE_INIT) {
			pr_warn("livedump: To uninitialize livedump the current state must be at least "
			    "LIVEDUMP_STATE_INIT.\n");
			return -EINVAL;
		}
		do_uninit();
		break;
	default:
		return -ENOIOCTLCMD;
	}

	if (ret == 0)
		livedump_state = cmd;

	return ret;
}

/* sysfs */

static struct kobject *livedump_root_kobj;

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int new_state, ret;

	ret = kstrtoint(buf, 10, &new_state);
	if (ret < 0)
		return -EINVAL;

	if (new_state < LIVEDUMP_STATE_UNDEFINED || new_state > LIVEDUMP_STATE_UNINIT)
		return -ENOIOCTLCMD;

	ret = livedump_change_state(new_state);
	if (ret < 0)
		return ret;

	livedump_state = new_state;
	return count;
}

static ssize_t state_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	ssize_t count = 0;

	count += sprintf(buf, "%u\n\n", livedump_state);
	count += sprintf(buf+count, "LIVEDUMP_STATE_UNDEFINED = 0\n");
	count += sprintf(buf+count, "LIVEDUMP_STATE_INIT = 1\n");
	count += sprintf(buf+count, "LIVEDUMP_STATE_START = 2\n");
	count += sprintf(buf+count, "LIVEDUMP_STATE_SWEEP = 3\n");
	count += sprintf(buf+count, "LIVEDUMP_STATE_FINISH = 4\n");
	count += sprintf(buf+count, "LIVEDUMP_STATE_UNINIT = 5\n");
	buf[count] = '\0';
	return count;
}

static ssize_t output_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int len;

	switch (livedump_state) {
	case LIVEDUMP_STATE_UNDEFINED:
	case LIVEDUMP_STATE_UNINIT:
		break;
	default:
		pr_warn("livedump: you cannot change the output in current state of livedump.\n");
		return -EINVAL;
	}

	len = strlcpy(livedump_conf.bdevpath, buf, sizeof(livedump_conf.bdevpath));
	if (len == 0 || len >= sizeof(livedump_conf.bdevpath))
		return -EINVAL;
	/* remove the newline character */
	livedump_conf.bdevpath[len-1] = '\0';	

	return count;
}

static ssize_t output_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", livedump_conf.bdevpath);
}

static struct kobj_attribute state_kobj_attr = __ATTR_RW(state);
static struct kobj_attribute output_kobj_attr = __ATTR_RW(output);
static struct attribute *livedump_attrs[] = {
	&state_kobj_attr.attr,
	&output_kobj_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(livedump);

static int livedump_exit(struct notifier_block *_, unsigned long __, void *___)
{
	if (livedump_root_kobj)
		kobject_put(livedump_root_kobj);
	do_uninit();
	return NOTIFY_DONE;
}
static struct notifier_block livedump_nb = {
	.notifier_call = livedump_exit
};

static int __init livedump_init(void)
{
	int ret;

	livedump_root_kobj = kobject_create_and_add("livedump", kernel_kobj);
	if (!livedump_root_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(livedump_root_kobj, *livedump_groups);
	if (ret) {
		livedump_exit(NULL, 0, NULL);
		return ret;
	}

	ret = register_reboot_notifier(&livedump_nb);
	if (WARN_ON(ret)) {
		livedump_exit(NULL, 0, NULL);
		return ret;
	}

	livedump_conf.bdevpath[0] = '\0';
	livedump_state = 0;

	return 0;
}

module_init(livedump_init);
