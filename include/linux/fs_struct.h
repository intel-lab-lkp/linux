/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H

#include <linux/sched.h>
#include <linux/path.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>

struct fs_struct {
	int users;
	seqlock_t seq;
	int umask;
	int in_exec;
	struct path root, pwd;
} __randomize_layout;

extern struct kmem_cache *fs_cachep;

extern void exit_fs(struct task_struct *);
extern void set_fs_root(struct fs_struct *, const struct path *);
extern void set_fs_pwd(struct fs_struct *, const struct path *);
extern struct fs_struct *copy_fs_struct(struct fs_struct *);
extern void free_fs_struct(struct fs_struct *);
extern int unshare_fs_struct(void);

static inline void get_fs_root(struct fs_struct *fs, struct path *root)
{
	read_seqlock_excl(&fs->seq);
	*root = fs->root;
	path_get(root);
	read_sequnlock_excl(&fs->seq);
}

static inline void get_fs_pwd(struct fs_struct *fs, struct path *pwd)
{
	read_seqlock_excl(&fs->seq);
	*pwd = fs->pwd;
	path_get(pwd);
	read_sequnlock_excl(&fs->seq);
}

/* Return true if fs->pwd matches the given pwd */
static inline bool fs_pwd_equal(struct fs_struct *fs, struct path *pwd)
{
	bool match;

	read_seqlock_excl(&fs->seq);
	match = (fs->pwd.dentry == pwd->dentry) && (fs->pwd.mnt == pwd->mnt);
	read_sequnlock_excl(&fs->seq);
	return match;
}

extern bool current_chrooted(void);

static inline int current_umask(void)
{
	return current->fs->umask;
}

#endif /* _LINUX_FS_STRUCT_H */
