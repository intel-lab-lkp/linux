/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Audit helpers
 *
 * Copyright © 2023 Microsoft Corporation
 */

#ifndef _SECURITY_LANDLOCK_AUDIT_H
#define _SECURITY_LANDLOCK_AUDIT_H

#include <linux/audit.h>
#include <linux/lsm_audit.h>

#include "ruleset.h"

enum landlock_operation {
	LANDLOCK_OP_PTRACE = 1,
	LANDLOCK_OP_PTRACE_TRACEME,
	LANDLOCK_OP_MOUNT,
	LANDLOCK_OP_MOVE_MOUNT,
	LANDLOCK_OP_UMOUNT,
	LANDLOCK_OP_REMOUNT,
	LANDLOCK_OP_PIVOT_ROOT,
	LANDLOCK_OP_MKDIR,
	LANDLOCK_OP_MKNOD,
	LANDLOCK_OP_SYMLINK,
	LANDLOCK_OP_UNLINK,
	LANDLOCK_OP_RMDIR,
	LANDLOCK_OP_TRUNCATE,
	LANDLOCK_OP_OPEN,
};

enum landlock_permission {
	LANDLOCK_PERM_PTRACE = 1,
	LANDLOCK_PERM_FS_LAYOUT,
};

struct landlock_request {
	const enum landlock_operation operation;
	const enum landlock_permission missing_permission;
	access_mask_t missing_access;
	u64 youngest_domain;
	struct common_audit_data audit;
};

#ifdef CONFIG_AUDIT

void landlock_log_create_ruleset(struct landlock_ruleset *const ruleset);
void landlock_log_restrict_self(struct landlock_ruleset *const domain,
				struct landlock_ruleset *const ruleset);
void landlock_log_release_ruleset(const struct landlock_ruleset *const ruleset);

int landlock_log_request(
	const int error, struct landlock_request *const request,
	const struct landlock_ruleset *const domain,
	const access_mask_t access_request,
	const layer_mask_t (*const layer_masks)[LANDLOCK_NUM_ACCESS_FS]);

#else /* CONFIG_AUDIT */

static inline void
landlock_log_create_ruleset(struct landlock_ruleset *const ruleset)
{
}

static inline void
landlock_log_restrict_self(struct landlock_ruleset *const domain,
			   struct landlock_ruleset *const ruleset)
{
}

static inline void
landlock_log_release_ruleset(const struct landlock_ruleset *const ruleset)
{
}

static inline int landlock_log_request(
	const int error, struct landlock_request *const request,
	const struct landlock_ruleset *const domain,
	const access_mask_t access_request,
	const layer_mask_t (*const layer_masks)[LANDLOCK_NUM_ACCESS_FS])
{
	return error;
}

#endif /* CONFIG_AUDIT */

#endif /* _SECURITY_LANDLOCK_AUDIT_H */
