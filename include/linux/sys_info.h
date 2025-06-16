/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SYS_INFO_H
#define _LINUX_SYS_INFO_H

/*
 * SYS_SHOW_ALL_PRINTK_MSG is for panic case only, as it needs special
 * handling which only fits panic case.
 */
#define SYS_SHOW_TASK_INFO		0x00000001
#define SYS_SHOW_MEM_INFO		0x00000002
#define SYS_SHOW_TIMER_INFO		0x00000004
#define SYS_SHOW_LOCK_INFO		0x00000008
#define SYS_SHOW_FTRACE_INFO		0x00000010
#define SYS_SHOW_ALL_PRINTK_MSG		0x00000020
#define SYS_SHOW_ALL_CPU_BT		0x00000040
#define SYS_SHOW_BLOCKED_TASKS		0x00000080

extern void sys_show_info(unsigned long info_mask);

#endif	/* _LINUX_SYS_INFO_H */
