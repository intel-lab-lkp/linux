/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SPAWN_TEMPLATE_H
#define _LINUX_SPAWN_TEMPLATE_H

#include <linux/elf.h>
#include <linux/fs.h>
#include <linux/refcount.h>

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

struct spawn_exec_template {
	refcount_t refcount;
	struct spawn_template_file_key exec_key;
	struct elfhdr exec_ehdr;
	struct elf_phdr *exec_phdrs;
	unsigned int exec_phnum;
};

void spawn_template_fill_file_key(struct file *file,
				  struct spawn_template_file_key *key);
bool spawn_template_file_key_matches(struct file *file,
				     const struct spawn_template_file_key *key);

#ifdef CONFIG_BINFMT_ELF
int spawn_exec_template_create(struct file *file,
			       struct spawn_exec_template **out);
struct spawn_exec_template *
spawn_exec_template_get(struct spawn_exec_template *tmpl);
void spawn_exec_template_put(struct spawn_exec_template *tmpl);
bool spawn_exec_template_matches(struct spawn_exec_template *tmpl,
				 struct file *file);
#else
static inline int spawn_exec_template_create(struct file *file,
					     struct spawn_exec_template **out)
{
	(void)file;
	(void)out;
	return -ENOEXEC;
}

static inline void spawn_exec_template_put(struct spawn_exec_template *tmpl)
{
	(void)tmpl;
}

static inline struct spawn_exec_template *
spawn_exec_template_get(struct spawn_exec_template *tmpl)
{
	return tmpl;
}

static inline bool spawn_exec_template_matches(struct spawn_exec_template *tmpl,
					       struct file *file)
{
	(void)tmpl;
	(void)file;
	return false;
}
#endif

#endif /* _LINUX_SPAWN_TEMPLATE_H */
