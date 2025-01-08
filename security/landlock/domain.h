/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Domain management
 *
 * Copyright © 2016-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 * Copyright © 2024-2025 Microsoft Corporation
 */

#ifndef _SECURITY_LANDLOCK_DOMAIN_H
#define _SECURITY_LANDLOCK_DOMAIN_H

#include <linux/cred.h>
#include <linux/landlock.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/time64.h>

#include "access.h"
#include "object.h"

enum landlock_log_status {
	LANDLOCK_LOG_PENDING = 0,
	LANDLOCK_LOG_RECORDED,
	LANDLOCK_LOG_DISABLED,
};

/**
 * struct landlock_details - Dommain's creation information
 *
 * Rarely accessed, mainly when logging the first domain's denial.
 *
 * The contained pointers are initialized at the domain creation time and never
 * changed again.  Contrary to most other Landlock object types, this one is
 * not allocated with GFP_KERNEL_ACCOUNT because its size may not be under the
 * caller's control (e.g. unknown exe_path) and the data is not explicitly
 * requested nor used by tasks.
 */
struct landlock_details {
	/**
	 * @creation: Time of the domain creation (i.e. syscall entry as used
	 * in audit context if available).
	 */
	struct timespec64 creation;
	/**
	 * @cred: Credential of the task that initially restricted itself, at
	 * creation time.
	 */
	const struct cred *cred;
	/**
	 * @pid: PID of the task that initially restricted itself.  It still
	 * identifies the same task.
	 */
	struct pid *pid;
	/**
	 * @exe_object: Landlock object tracking the executable binary that
	 * restricted itself, for its whole lifetime.
	 */
	struct landlock_object *exe_object;
	/**
	 * @exe_ino: Inode number cache of the executable binary.  This should
	 * only be read if @exe_object is not NULL, while holding the related
	 * inode.  This is useful to avoid locking @exe_object or the
	 * underlying inode.
	 */
	ino_t exe_ino;
	/**
	 * @exe_dev: Device number cache of the executable binary.  This should
	 * only be read if @exe_object is not NULL, while holding the related
	 * inode.  This is useful to avoid locking @exe_object or the
	 * underlying inode.
	 */
	dev_t exe_dev;
	/**
	 * @comm: Command line of the task that initially restricted itself, at
	 * creation time.  Always NULL terminated.
	 */
	char comm[TASK_COMM_LEN];
	/**
	 * @exe_path: Executable path of the task that initially restricted
	 * itself, at creation time.  Always NULL terminated.
	 */
	char exe_path[];
};

/**
 * struct landlock_hierarchy - Node in a domain hierarchy
 */
struct landlock_hierarchy {
	/**
	 * @parent: Pointer to the parent node, or NULL if it is a root
	 * Landlock domain.
	 */
	struct landlock_hierarchy *parent;
	/**
	 * @usage: Number of potential children domains plus their parent
	 * domain.
	 */
	refcount_t usage;

#ifdef CONFIG_AUDIT
	/**
	 * @log_status: Whether this domain should be logged or not.  Because
	 * concurrent log entries may be created at the same time, it is still
	 * possible to have several domain records of the same domain.
	 */
	enum landlock_log_status log_status;
	/**
	 * @num_denials: Number of access requests denied by this domain.
	 */
	atomic64_t num_denials;
	/**
	 * @id: Landlock domain ID, sets once at domain creation time.
	 */
	u64 id;
	/**
	 * @details: Information about the related domain.
	 */
	const struct landlock_details *details;
#endif /* CONFIG_AUDIT */
};

#ifdef CONFIG_AUDIT

int landlock_init_current_hierarchy(struct landlock_hierarchy *const hierarchy);

deny_masks_t
landlock_get_deny_masks(const access_mask_t all_existing_optional_access,
			const access_mask_t optional_access,
			const layer_mask_t (*const layer_masks)[],
			size_t layer_masks_size);

#else /* CONFIG_AUDIT */

static inline int
landlock_init_current_hierarchy(struct landlock_hierarchy *const hierarchy)
{
	return 0;
}

#endif /* CONFIG_AUDIT */

#endif /* _SECURITY_LANDLOCK_DOMAIN_H */
