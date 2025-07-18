/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_RT_IPC_H__
#define __LINUX_RT_IPC_H__

#ifdef CONFIG_RT_IPC
#include <asm/rt_ipc.h>

void rt_ipc_deregister(struct task_struct *tsk);
#endif

#endif /* __LINUX_RT_IPC_H__ */