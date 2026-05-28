/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SPAWN_TEMPLATE_H
#define _LINUX_SPAWN_TEMPLATE_H

#include <linux/fs.h>

struct spawn_template_file_key {
	dev_t dev;
	ino_t ino;
	loff_t size;
	umode_t mode;
	kuid_t uid;
	kgid_t gid;
	u64 ctime_sec;
	u64 ctime_nsec;
	u64 mtime_sec;
	u64 mtime_nsec;
};

void spawn_template_fill_file_key(struct file *file,
				  struct spawn_template_file_key *key);
bool spawn_template_file_key_matches(struct file *file,
				     const struct spawn_template_file_key *key);

#endif /* _LINUX_SPAWN_TEMPLATE_H */
