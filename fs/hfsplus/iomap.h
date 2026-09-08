/* SPDX-License-Identifier: GPL-2.0 */
/*
 * iomap callback declarations for the hfsplus filesystem
 */

#ifndef _LINUX_HFSPLUS_IOMAP_H
#define _LINUX_HFSPLUS_IOMAP_H

extern const struct iomap_ops hfsplus_iomap_ops;
extern const struct iomap_ops hfsplus_write_iomap_ops;
extern const struct iomap_writeback_ops hfsplus_writeback_ops;
extern const struct iomap_dio_ops hfsplus_write_dio_ops;

int hfsplus_iomap_cont_expand(struct inode *inode, loff_t size);
int hfsplus_iomap_swap_activate(struct swap_info_struct *sis,
				struct file *file, sector_t *span);

#endif /* _LINUX_HFSPLUS_IOMAP_H */
