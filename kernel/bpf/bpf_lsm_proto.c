// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Google LLC.
 */

#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/bpf_lsm.h>

/*
 * Strong definition of the mmap_file() BPF LSM hook. The __nullable suffix on
 * the struct file pointer parameter name marks it as PTR_MAYBE_NULL. This
 * explicitly enforces that BPF LSM programs check for NULL before attempting to
 * dereference it.
 */
noinline int bpf_lsm_mmap_file(struct file *file__nullable, unsigned long reqprot,
			       unsigned long prot, unsigned long flags)
{
	return 0;
}

/*
 * Strong definition of the inode_init_security() BPF LSM hook. Both the
 * qstr and the xattr array are NULL for some callers, so the __nullable
 * suffix marks it as PTR_MAYBE_NULL. BPF LSM programs have to check before
 * dereferencing them or handing them to bpf_inode_init_xattr().
 */
noinline int bpf_lsm_inode_init_security(struct inode *inode, struct inode *dir,
					 const struct qstr *qstr__nullable,
					 struct xattr *xattrs__nullable,
					 int *xattr_count)
{
	return -EOPNOTSUPP;
}

/*
 * Strong definition of the path_setattr() BPF LSM hook.  @dentry is the
 * inode whose attributes are changed and can differ from @path->dentry for
 * filesystems that forward the change.  The file argument is only set for
 * file descriptor-based operations and is NULL for path-based and in-kernel
 * callers, so mark it as PTR_MAYBE_NULL.
 */
noinline int bpf_lsm_path_setattr(const struct mnt_idmap *idmap,
				  const struct path *path,
				  struct dentry *dentry,
				  struct iattr *attr,
				  struct file *file__nullable)
{
	return 0;
}

/*
 * Strong definitions of the inode xattr BPF LSM hooks.  The file argument is
 * only set for file descriptor-based operations and is NULL for path-based
 * and in-kernel callers, so mark it as PTR_MAYBE_NULL.
 */
noinline int bpf_lsm_inode_setxattr(const struct mnt_idmap *idmap,
				    const struct path *path, const char *name,
				    const void *value, size_t size, int flags,
				    struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_getxattr(const struct path *path, const char *name,
				    struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_listxattr(const struct path *path,
				     struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_removexattr(const struct mnt_idmap *idmap,
				       const struct path *path,
				       const char *name,
				       struct file *file__nullable)
{
	return 0;
}

/*
 * Strong definitions of the inode POSIX ACL BPF LSM hooks.  The file argument
 * is only set for file descriptor-based operations and is NULL for path-based
 * and in-kernel callers, so mark it as PTR_MAYBE_NULL.
 */
noinline int bpf_lsm_inode_set_acl(const struct mnt_idmap *idmap,
				   const struct path *path,
				   const char *acl_name, struct posix_acl *kacl,
				   struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_get_acl(const struct mnt_idmap *idmap,
				   const struct path *path,
				   const char *acl_name,
				   struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_remove_acl(const struct mnt_idmap *idmap,
				      const struct path *path,
				      const char *acl_name,
				      struct file *file__nullable)
{
	return 0;
}

/*
 * Strong definitions of the inode fileattr BPF LSM hooks.  The file argument
 * is only set for file descriptor-based operations and is NULL for path-based
 * and in-kernel callers, so mark it as PTR_MAYBE_NULL.
 */
noinline int bpf_lsm_inode_file_setattr(const struct path *path,
					struct file_kattr *fa,
					struct file *file__nullable)
{
	return 0;
}

noinline int bpf_lsm_inode_file_getattr(const struct path *path,
					struct file_kattr *fa,
					struct file *file__nullable)
{
	return 0;
}
