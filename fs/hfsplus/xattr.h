/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/fs/hfsplus/xattr.h
 *
 * Vyacheslav Dubeyko <slava@dubeyko.com>
 *
 * Logic of processing extended attributes
 */

#ifndef _LINUX_HFSPLUS_XATTR_H
#define _LINUX_HFSPLUS_XATTR_H

#include <linux/xattr.h>

/*
 * Internal xattrs of Linux HFS+ driver. Timestamps beyond
 * HFS_MAX_TIMESTAMP_SECS (February 2040) cannot be represented by
 * 32-bit on-disk fields. Such timestamp is stored as the maximal value
 * (HFSPLUS_EXT_TIMESTAMP_MARK) in the catalog record and the real value
 * is kept in the xattr as big-endian 64-bit seconds since
 * 00:00 GMT, Jan. 1, 1970.
 */
#define XATTR_LINUX_HFS_PREFIX "linux_hfs."
#define XATTR_LINUX_HFS_PREFIX_LEN \
	(sizeof(XATTR_LINUX_HFS_PREFIX) - 1)
#define XATTR_LINUX_HFS_NAME_MAX	64
#define XATTR_LINUX_CREATE_DATE_NAME "create_date_u64"
#define XATTR_LINUX_CONTENT_MOD_DATE_NAME "content_mod_date_u64"
#define XATTR_LINUX_ATTRIBUTE_MOD_DATE_NAME "attribute_mod_date_u64"
#define XATTR_LINUX_ACCESS_DATE_NAME "access_date_u64"
#define XATTR_LINUX_BACKUP_DATE_NAME "backup_date_u64"

extern const struct xattr_handler hfsplus_xattr_osx_handler;
extern const struct xattr_handler hfsplus_xattr_user_handler;
extern const struct xattr_handler hfsplus_xattr_trusted_handler;
extern const struct xattr_handler hfsplus_xattr_security_handler;

extern const struct xattr_handler * const hfsplus_xattr_handlers[];

int __hfsplus_setxattr(struct inode *inode, const char *name,
			const void *value, size_t size, int flags);

int hfsplus_setxattr(struct inode *inode, const char *name,
		     const void *value, size_t size, int flags,
		     const char *prefix, size_t prefixlen);
ssize_t hfsplus_getxattr(struct inode *inode, const char *name,
			 void *value, size_t size,
			 const char *prefix, size_t prefixlen);

int hfsplus_get_timestamp_xattr(struct super_block *sb, u32 cnid,
				const char *name, time64_t *ts);
int hfsplus_set_timestamp_xattr(struct inode *inode, u32 cnid,
				const char *name, time64_t ts);
int hfsplus_remove_timestamp_xattr(struct inode *inode, const char *name);

ssize_t hfsplus_listxattr(struct dentry *dentry, char *buffer, size_t size);

int hfsplus_init_security(struct inode *inode, struct inode *dir,
				const struct qstr *qstr);

#endif
