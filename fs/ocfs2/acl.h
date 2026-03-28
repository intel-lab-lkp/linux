/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acl.h
 *
 * Copyright (C) 2004, 2008 Oracle.  All rights reserved.
 */

#ifndef OCFS2_ACL_H
#define OCFS2_ACL_H

#include <linux/posix_acl_xattr.h>

/*
 * ocfs2_init_acl() reads the parent directory's default ACL while
 * ocfs2_mknod() already holds a transaction handle. ocfs2_xattr_set()
 * takes ip_xattr_sem on the target inode before starting a transaction,
 * so tell lockdep that the parent directory lookup is a higher-level
 * acquisition in the parent-child hierarchy.
 */
#define OCFS2_XATTR_SEM_OWNER_PARENT	1

struct ocfs2_acl_entry {
	__le16 e_tag;
	__le16 e_perm;
	__le32 e_id;
};

struct posix_acl *ocfs2_iop_get_acl(struct inode *inode, int type, bool rcu);
int ocfs2_iop_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		      struct posix_acl *acl, int type);
int ocfs2_acl_chmod(struct inode *inode, struct buffer_head *bh);
int ocfs2_init_acl(handle_t *handle, struct inode *inode, struct inode *dir,
		   struct buffer_head *di_bh, struct buffer_head *dir_bh,
		   struct ocfs2_alloc_context *meta_ac,
		   struct ocfs2_alloc_context *data_ac);

#endif /* OCFS2_ACL_H */
