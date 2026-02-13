/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __DLMPFS_H__
#define __DLMPFS_H__

#include <linux/fs.h>
#include <linux/dlm.h>

struct dlmls;
struct dlmlk;

#define DLMPFS_NUM_RESNAME_LEN 32

struct dlmpfs_mount_opts {
	umode_t mode;
};

struct dlmpfs_fs_info {
	struct dlmpfs_mount_opts mount_opts;
	unsigned int last_inum;
	char clname[DLM_RESNAME_MAXLEN];
	char lsname[DLM_RESNAME_MAXLEN];
	struct dlmls *ls;
};

extern const struct file_operations dlmpfs_file_operations;
extern const struct inode_operations dlmpfs_file_inode_operations;

void dlmpfs_fill_resname_num(const char *prefix, char *resname,
			     unsigned int num);
struct dlmlk *dlmlk_alloc(struct dlmls *ls, const void *name, size_t namelen);
void dlmlk_free(struct dlmlk *lk);
void dlmlk_free_nowait(struct dlmlk *lk);
void dlmlk_convert(struct dlmlk *lk, int mode, unsigned long flags);
int dlmlk_convert_interuptible(struct dlmlk *lk, int mode, unsigned long flags);
const struct dlm_lksb *dlmlk_sb(struct dlmlk *lk);
int dlmlk_grmode(struct dlmlk *lk);
int dlmls_new(const char *lsname, const char *clname,
	      uint32_t flags, int lvblen,
	      const struct dlm_lockspace_ops *ops,
	      void *ops_arg, int *ops_result,
	      struct dlmls **ls_ret);
void dlmls_release(struct dlmls *ls, uint32_t flags);
int dlmpfs_get_path(struct dentry *dentry, char **path,
		    size_t *pathlen, char **page_buf);

int dlmplk_lock(struct dlmls *ls, u64 number, struct file *file,
		int cmd, struct file_lock *fl);
int dlmplk_unlock(struct dlmls *ls, u64 number, struct file *file,
		  struct file_lock *fl);
int dlmplk_cancel(struct dlmls *ls, u64 number, struct file *file,
		  struct file_lock *fl);
int dlmplk_get(struct dlmls *ls, u64 number, struct file *file,
	       struct file_lock *fl);
static inline void *dlmlk_lvb(struct dlmlk *lk)
{
	return (void *)dlmlk_sb(lk)->sb_lvbptr;
}

static inline struct dlmpfs_fs_info *DLMPFS_FSI(const struct inode *inode)
{
	return inode->i_sb->s_fs_info;
}

#endif /* __DLMPFS_H__ */
