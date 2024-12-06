/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_IO_H
#define _FS_CEPH_IO_H

#include <linux/compiler_attributes.h> // for __must_check

__must_check int ceph_start_io_read(struct inode *inode);
void ceph_end_io_read(struct inode *inode);
__must_check int ceph_start_io_write(struct inode *inode);
void ceph_end_io_write(struct inode *inode);
__must_check int ceph_start_io_direct(struct inode *inode);
void ceph_end_io_direct(struct inode *inode);

#endif /* FS_CEPH_IO_H */
