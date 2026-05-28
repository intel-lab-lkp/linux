/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SPAWN_TEMPLATE_H
#define _UAPI_LINUX_SPAWN_TEMPLATE_H

#include <linux/openat2.h>
#include <linux/types.h>

#define SPAWN_TEMPLATE_CREATE_CLOEXEC		(1ULL << 0)
#define SPAWN_TEMPLATE_SPAWN_INHERIT_FDS	(1ULL << 0)

enum spawn_template_action_type {
	SPAWN_TEMPLATE_ACTION_CLOSE = 0,
	SPAWN_TEMPLATE_ACTION_DUP2 = 1,
	SPAWN_TEMPLATE_ACTION_FCHDIR = 2,
	SPAWN_TEMPLATE_ACTION_OPEN = 3,
	SPAWN_TEMPLATE_ACTION_CLOSE_RANGE = 4,
	SPAWN_TEMPLATE_ACTION_SIGMASK = 5,
	SPAWN_TEMPLATE_ACTION_SIGDEFAULT = 6,
};

struct spawn_template_action {
	__u32 type;
	__u32 flags;
	__s32 fd;
	__s32 newfd;
	__aligned_u64 arg;
};

struct spawn_template_open {
	__aligned_u64 path;
	struct open_how how;
};

struct spawn_template_sigset {
	__aligned_u64 sigset;
	__u64 sigsetsize;
};

struct spawn_template_create_args {
	__aligned_u64 flags;
	__s32 execfd;
	__u32 exec_flags;
	__aligned_u64 filename;
	__aligned_u64 actions;
	__aligned_u64 actions_len;
	__aligned_u64 reserved[4];
};

struct spawn_template_spawn_args {
	__aligned_u64 flags;
	__aligned_u64 pidfd;
	__aligned_u64 argv;
	__aligned_u64 envp;
	__aligned_u64 actions;
	__aligned_u64 actions_len;
	__aligned_u64 reserved[4];
};

#define SPAWN_TEMPLATE_CREATE_ARGS_SIZE_VER0	72
#define SPAWN_TEMPLATE_SPAWN_ARGS_SIZE_VER0	80

#endif /* _UAPI_LINUX_SPAWN_TEMPLATE_H */
