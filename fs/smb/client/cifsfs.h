/* SPDX-License-Identifier: LGPL-2.1 */
/*
 *
 *   Copyright (c) International Business Machines  Corp., 2002, 2007
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 */

#ifndef _CIFSFS_H
#define _CIFSFS_H

#include <linux/hash.h>

#define ROOT_I 2

/*
 * ino_t is 32-bits on 32-bit arch. We have to squash the 64-bit value down
 * so that it will fit. We use hash_64 to convert the value to 31 bits, and
 * then add 1, to ensure that we don't end up with a 0 as the value.
 */
static inline ino_t
cifs_uniqueid_to_ino_t(u64 fileid)
{
	if ((sizeof(ino_t)) < (sizeof(u64)))
		return (ino_t)hash_64(fileid, (sizeof(ino_t) * 8) - 1) + 1;

	return (ino_t)fileid;

}

static inline void cifs_set_time(struct dentry *dentry, unsigned long time)
{
	dentry->d_fsdata = (void *) time;
}

static inline unsigned long cifs_get_time(struct dentry *dentry)
{
	return (unsigned long) dentry->d_fsdata;
}

extern struct file_system_type cifs_fs_type, smb3_fs_type;
extern const struct address_space_operations cifs_addr_ops;
extern const struct address_space_operations cifs_addr_ops_smallbuf;

/* Functions related to super block operations */

/* Functions related to inodes */
extern const struct inode_operations cifs_dir_inode_ops;

extern const struct inode_operations cifs_file_inode_ops;
extern const struct inode_operations cifs_symlink_inode_ops;
extern const struct inode_operations cifs_namespace_inode_operations;

/* Functions related to files and directories */
extern const struct netfs_request_ops cifs_req_ops;
extern const struct file_operations cifs_file_ops;
extern const struct file_operations cifs_file_direct_ops; /* if directio mnt */
extern const struct file_operations cifs_file_strict_ops; /* if strictio mnt */
extern const struct file_operations cifs_file_nobrl_ops; /* no brlocks */
extern const struct file_operations cifs_file_direct_nobrl_ops;
extern const struct file_operations cifs_file_strict_nobrl_ops;
extern const struct file_operations cifs_dir_ops;

/* Functions related to dir entries */
extern const struct dentry_operations cifs_dentry_ops;
extern const struct dentry_operations cifs_ci_dentry_ops;

/* Functions related to symlinks */

#ifdef CONFIG_CIFS_XATTR
extern const struct xattr_handler * const cifs_xattr_handlers[];
#else
# define cifs_xattr_handlers NULL
# define cifs_listxattr NULL
#endif

struct smb3_fs_context;

#ifdef CONFIG_CIFS_NFSD_EXPORT
extern const struct export_operations cifs_export_ops;
#endif /* CONFIG_CIFS_NFSD_EXPORT */

/* when changing internal version - update following two lines at same time */
#define SMB3_PRODUCT_BUILD 56
#define CIFS_VERSION   "2.56"
#endif				/* _CIFSFS_H */
