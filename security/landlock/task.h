/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Ptrace hooks
 *
 * Copyright © 2017-2019 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2019 ANSSI
 */

#ifndef _SECURITY_LANDLOCK_TASK_H
#define _SECURITY_LANDLOCK_TASK_H

#include <linux/ipc.h>
#include <linux/types.h>

#include "cred.h"
#include "setup.h"

/**
 * enum landlock_sysv_ipc_kind - Kind of SysV IPC object backed by a blob
 *
 * @LANDLOCK_SYSV_IPC_UNSET: Blob has not been tagged by a Landlock IPC
 *	allocation hook.  This is the zero value used for sem and shm
 *	objects that Landlock does not currently scope, as well as for
 *	any future kind that has not yet been wired up.
 * @LANDLOCK_SYSV_IPC_MSG_QUEUE: Blob belongs to a SysV message queue.
 */
enum landlock_sysv_ipc_kind {
	LANDLOCK_SYSV_IPC_UNSET = 0,
	LANDLOCK_SYSV_IPC_MSG_QUEUE,
};

/**
 * struct landlock_kern_ipc_perm_security - IPC object security blob
 *
 * Enable provenance tracking of SysV IPC objects to scope IPC accesses.
 * The LSM core allocates a blob for every kern_ipc_perm regardless of the
 * underlying object kind (msg queue, semaphore, shared memory), so callers
 * that act on a subset of object kinds must consult @kind before
 * interpreting @owner_subject.
 */
struct landlock_kern_ipc_perm_security {
	/**
	 * @owner_subject: Landlock credential of the task that created the
	 * kernel IPC object.  Only meaningful when @kind is not
	 * %LANDLOCK_SYSV_IPC_UNSET.
	 */
	struct landlock_cred_security owner_subject;
	/**
	 * @kind: Kind of SysV IPC object this blob describes.  Set by the
	 * matching alloc hook; %LANDLOCK_SYSV_IPC_UNSET for objects whose
	 * kind Landlock does not currently track.
	 */
	enum landlock_sysv_ipc_kind kind;
};

static inline struct landlock_kern_ipc_perm_security *
landlock_kern_ipc_perm(const struct kern_ipc_perm *const perm)
{
	return perm->security + landlock_blob_sizes.lbs_ipc;
}

__init void landlock_add_task_hooks(void);

#endif /* _SECURITY_LANDLOCK_TASK_H */
