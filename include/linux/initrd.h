/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_INITRD_H
#define __LINUX_INITRD_H

/* 1 if it is not an error if initrd_start < memory_start */
extern int initrd_below_start_ok;

extern unsigned long initrd_start, initrd_end;
extern void free_initrd_mem(unsigned long, unsigned long);

#ifdef CONFIG_BLK_DEV_INITRD
extern void __init reserve_initrd_mem(void);
extern void wait_for_initramfs(void);
#else
static inline void __init reserve_initrd_mem(void) {}
static inline void wait_for_initramfs(void) {}
#endif

extern phys_addr_t phys_initrd_start;
extern unsigned long phys_initrd_size;

extern char __builtin_initramfs_start[];
extern unsigned long __builtin_initramfs_size;

void console_on_rootfs(void);

#endif /* __LINUX_INITRD_H */
