/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_RT_IPC_H
#define _ASM_X86_RT_IPC_H

#include <linux/signal_types.h>
#include <linux/spinlock_types.h>
#include <asm/ucontext.h>

#define RT_IPC_ACTIVATION_THREAD_NUM 8

typedef struct rt_ipc_info {
	size_t		write_size;	/* bytes to write */
	size_t		write_consumed;	/* bytes consumed by driver */
	uintptr_t	write_buffer;
	size_t		read_size;	/* bytes to read */
	size_t		read_consumed;	/* bytes consumed by driver */
	uintptr_t	read_buffer;
} rt_ipc_info_t;

struct rt_ipc_action {
	void (*entry)(unsigned cmd, rt_ipc_info_t *info);
	void (*restorer)(void);
	int flags;
};

struct rt_ipc_migrate_context {
	struct task_struct *group_leader;
	struct files_struct *files;
	struct fs_struct *fs;
	struct sighand_struct *sighand;
	struct signal_struct *signal;
	struct pid *thread_pid;
	pid_t pid;
	pid_t tgid;
	struct rseq *rseq;
	u32 rseq_sig;
	unsigned long rseq_event_mask;
	struct mm_struct *mm;
	struct mm_struct *active_mm;
	struct nsproxy *nsproxy;
	unsigned long min_flt;
	unsigned long maj_flt;
	unsigned long fsbase;
};

struct rt_ipc_context {
	struct pt_regs regs;
	unsigned long trap_nr;
	unsigned long error_code;
	unsigned long gs;
	unsigned long fs;
	unsigned long cr2;
};

struct rt_ipc_activation {
	struct list_head activation_link;

	size_t stack;
	struct rt_ipc_info __user *info;
	struct rt_ipc_action *act;
	struct task_struct *s;
	struct task_struct *c;

	struct rt_ipc_migrate_context context;
	struct rt_ipc_context server_ctx;
	struct rt_ipc_context client_ctx;
};

struct rt_ipc_frame {
	void __user *pretcode;
	struct rt_ipc_info info;
};

int rt_ipc_config_activation(struct task_struct *task);
int rt_ipc_migrate_thread(struct rt_ipc_activation *activation, unsigned int cmd, struct rt_ipc_info *info);

extern void rt_ipc_context_switch(struct task_struct *next);
#endif /* _ASM_X86_RT_IPC_H */