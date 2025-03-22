/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_INITRD_H
#define __LINUX_INITRD_H

#include <linux/init.h>
#include <linux/types.h>

#define INITRD_MINOR 250 /* shouldn't collide with /dev/ram* too soon ... */

/* starting block # of image */
extern int rd_image_start;

/* size of a single RAM disk */
extern unsigned long rd_size;

/* 1 if it is not an error if initrd_start < memory_start */
extern int initrd_below_start_ok;

/* free_initrd_mem always gets called with the next two as arguments.. */
extern unsigned long initrd_start, initrd_end;
extern void free_initrd_mem(unsigned long, unsigned long);

struct file;

#ifdef CONFIG_BLK_DEV_INITRD
extern void __init reserve_initrd_mem(void);
extern void wait_for_initramfs(void);

/*
 * Detect a filesystem on the initrd. You get 1 KiB (BLOCK_SIZE) of
 * data to work with. The offset of the block is specified in
 * initrd_fs_detect().
 *
 * @block_data: A pointer to BLOCK_SIZE of data
 *
 * Returns the size of the filesystem in bytes or 0, if the filesystem
 * was not detected.
 */
typedef size_t initrd_fs_detect_fn(void * const block_data);

struct initrd_detect_fs {
	initrd_fs_detect_fn *detect_fn;
	loff_t detect_byte_offset;
};

extern struct initrd_detect_fs __start_initrd_fs_detect[];
extern struct initrd_detect_fs __stop_initrd_fs_detect[];

/*
 * Add a filesystem detector for initrds. See the documentation of
 * initrd_fs_detect_fn above.
 */
#define initrd_fs_detect(fn, byte_offset)					\
	static const struct initrd_detect_fs __initrd_fs_detect_ ## fn		\
	__used __section("_initrd_fs_detect") =					\
		{ .detect_fn = fn, .detect_byte_offset = byte_offset}

#else
static inline void __init reserve_initrd_mem(void) {}
static inline void wait_for_initramfs(void) {}

#define initrd_fs_detect(detectfn)
#endif

extern phys_addr_t phys_initrd_start;
extern unsigned long phys_initrd_size;

extern char __initramfs_start[];
extern unsigned long __initramfs_size;

void console_on_rootfs(void);

#endif /* __LINUX_INITRD_H */
