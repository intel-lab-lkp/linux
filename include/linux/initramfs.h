/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_INITRAMFS_H
#define __LINUX_INITRAMFS_H

/* 1 if it is not an error if virt_external_initramfs_start < memory_start */
extern int initramfs_below_start_ok;

extern unsigned long virt_external_initramfs_start, virt_external_initramfs_end;
extern void free_initramfs_mem(unsigned long, unsigned long);

#ifdef CONFIG_INITRAMFS
extern void __init reserve_initramfs_mem(void);
extern void wait_for_initramfs(void);
#else
static inline void __init reserve_initramfs_mem(void) {}
static inline void wait_for_initramfs(void) {}
#endif

extern phys_addr_t phys_external_initramfs_start;
extern unsigned long phys_external_initramfs_size;

extern char __builtin_initramfs_start[];
extern unsigned long __builtin_initramfs_size;

#endif /* __LINUX_INITRAMFS_H */
