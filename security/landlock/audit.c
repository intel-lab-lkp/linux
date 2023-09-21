// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Audit helpers
 *
 * Copyright © 2023 Microsoft Corporation
 */

#include <linux/atomic.h>
#include <linux/audit.h>
#include <linux/lsm_audit.h>

#include "audit.h"
#include "cred.h"

atomic64_t ruleset_and_domain_counter = ATOMIC64_INIT(0);

static const char *op_to_string(enum landlock_operation operation)
{
	const char *const desc[] = {
		[0] = "",
		[LANDLOCK_OP_PTRACE] = "ptrace",
		[LANDLOCK_OP_PTRACE_TRACEME] = "ptrace_traceme",
		[LANDLOCK_OP_MOUNT] = "mount",
		[LANDLOCK_OP_MOVE_MOUNT] = "move_mount",
		[LANDLOCK_OP_UMOUNT] = "umount",
		[LANDLOCK_OP_REMOUNT] = "remount",
		[LANDLOCK_OP_PIVOT_ROOT] = "pivot_root",
		[LANDLOCK_OP_MKDIR] = "mkdir",
		[LANDLOCK_OP_MKNOD] = "mknod",
		[LANDLOCK_OP_SYMLINK] = "symlink",
		[LANDLOCK_OP_UNLINK] = "unlink",
		[LANDLOCK_OP_RMDIR] = "rmdir",
		[LANDLOCK_OP_TRUNCATE] = "truncate",
		[LANDLOCK_OP_OPEN] = "open",
	};

	if (WARN_ON_ONCE(operation < 0 || operation > ARRAY_SIZE(desc)))
		return "unknown";

	return desc[operation];
}

static const char *perm_to_string(enum landlock_permission permission)
{
	const char *const desc[] = {
		[0] = "",
		[LANDLOCK_PERM_PTRACE] = "ptrace",
		[LANDLOCK_PERM_FS_LAYOUT] = "fs_layout",
	};

	if (WARN_ON_ONCE(permission < 0 || permission > ARRAY_SIZE(desc)))
		return "unknown";

	return desc[permission];
}

#define BIT_INDEX(bit) HWEIGHT(bit - 1)

static void log_accesses(struct audit_buffer *const ab,
			 const access_mask_t accesses)
{
	const char *const desc[] = {
		[BIT_INDEX(LANDLOCK_ACCESS_FS_EXECUTE)] = "execute",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_WRITE_FILE)] = "write_file",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_READ_FILE)] = "read_file",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_READ_DIR)] = "read_dir",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_REMOVE_DIR)] = "remove_dir",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_REMOVE_FILE)] = "remove_file",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_CHAR)] = "make_char",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_DIR)] = "make_dir",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_REG)] = "make_reg",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_SOCK)] = "make_sock",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_FIFO)] = "make_fifo",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_BLOCK)] = "make_block",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_MAKE_SYM)] = "make_sym",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_REFER)] = "refer",
		[BIT_INDEX(LANDLOCK_ACCESS_FS_TRUNCATE)] = "truncate",
	};
	const unsigned long access_mask = accesses;
	unsigned long access_bit;
	bool is_first = true;

	BUILD_BUG_ON(ARRAY_SIZE(desc) != LANDLOCK_NUM_ACCESS_FS);

	for_each_set_bit(access_bit, &access_mask, ARRAY_SIZE(desc)) {
		audit_log_format(ab, "%s%s", is_first ? "" : ",",
				 desc[access_bit]);
		is_first = false;
	}
}

/* Inspired by dump_common_audit_data(). */
static void log_task(struct audit_buffer *const ab)
{
	/* 16 bytes (TASK_COMM_LEN) */
	char comm[sizeof(current->comm)];

	/*
	 * Uses task_pid_nr() instead of task_tgid_nr() because of how
	 * credentials and Landlock work.
	 */
	audit_log_format(ab, "tid=%d comm=", task_pid_nr(current));
	audit_log_untrustedstring(ab,
				  memcpy(comm, current->comm, sizeof(comm)));
}

void landlock_log_create_ruleset(struct landlock_ruleset *const ruleset)
{
	struct audit_buffer *ab;

	WARN_ON_ONCE(ruleset->id);

	ab = audit_log_start(audit_context(), GFP_ATOMIC, AUDIT_LANDLOCK);
	if (!ab)
		/* audit_log_lost() call */
		return;

	ruleset->id = atomic64_inc_return(&ruleset_and_domain_counter);
	log_task(ab);
	audit_log_format(ab,
			 " op=create-ruleset ruleset=%llu handled_access_fs=",
			 ruleset->id);
	log_accesses(ab, ruleset->fs_access_masks[ruleset->num_layers - 1]);
	audit_log_end(ab);
}

void landlock_log_restrict_self(struct landlock_ruleset *const domain,
				struct landlock_ruleset *const ruleset)
{
	struct audit_buffer *ab;

	WARN_ON_ONCE(domain->id);
	WARN_ON_ONCE(!ruleset->id);

	ab = audit_log_start(audit_context(), GFP_ATOMIC, AUDIT_LANDLOCK);
	if (!ab)
		/* audit_log_lost() call */
		return;

	domain->hierarchy->id =
		atomic64_inc_return(&ruleset_and_domain_counter);
	log_task(ab);
	audit_log_format(ab, " op=restrict-self domain=%llu ruleset=%llu",
			 domain->hierarchy->id, ruleset->id);
	audit_log_format(
		ab, " parent=%llu",
		domain->hierarchy->parent ? domain->hierarchy->parent->id : 0);
	audit_log_end(ab);
}

/*
 * This is useful to know when a domain or a ruleset will never show again in
 * the audit log.
 */
void landlock_log_release_ruleset(const struct landlock_ruleset *const ruleset)
{
	struct audit_buffer *ab;
	const char *name;
	u64 id;

	ab = audit_log_start(audit_context(), GFP_ATOMIC, AUDIT_LANDLOCK);
	if (!ab)
		/* audit_log_lost() call */
		return;

	/* It should either be a domain or a ruleset. */
	if (ruleset->hierarchy) {
		name = "domain";
		id = ruleset->hierarchy->id;
		WARN_ON_ONCE(ruleset->id);
	} else {
		name = "ruleset";
		id = ruleset->id;
	}
	WARN_ON_ONCE(!id);

	/*
	 * Because this might be called by kernel threads, logging
	 * related task information with log_task() would be useless.
	 */
	audit_log_format(ab, "op=release-%s %s=%llu", name, name, id);
	audit_log_end(ab);
}

/* Update request.youngest_domain and request.missing_access */
static void
update_request(struct landlock_request *const request,
	       const struct landlock_ruleset *const domain,
	       const access_mask_t access_request,
	       const layer_mask_t (*const layer_masks)[LANDLOCK_NUM_ACCESS_FS])
{
	const unsigned long access_req = access_request;
	unsigned long access_bit;
	long youngest_denied_layer = -1;
	const struct landlock_hierarchy *node = domain->hierarchy;
	size_t i;

	WARN_ON_ONCE(request->youngest_domain);
	WARN_ON_ONCE(request->missing_access);

	if (!access_request) {
		/* No missing accesses. */
		request->youngest_domain = node->id;
		return;
	}

	if (WARN_ON_ONCE(!layer_masks))
		return;

	for_each_set_bit(access_bit, &access_req, ARRAY_SIZE(*layer_masks)) {
		long domain_layer;

		if (!(*layer_masks)[access_bit])
			continue;

		domain_layer = __fls((*layer_masks)[access_bit]);

		/*
		 * Gets the access rights that are missing from
		 * the youngest (i.e. closest) domain.
		 */
		if (domain_layer == youngest_denied_layer) {
			request->missing_access |= BIT_ULL(access_bit);
		} else if (domain_layer > youngest_denied_layer) {
			youngest_denied_layer = domain_layer;
			request->missing_access = BIT_ULL(access_bit);
		}
	}

	WARN_ON_ONCE(!request->missing_access);
	WARN_ON_ONCE(youngest_denied_layer < 0);

	/* Gets the nearest domain ID that denies request.missing_access */
	for (i = domain->num_layers - youngest_denied_layer - 1; i > 0; i--)
		node = node->parent;
	request->youngest_domain = node->id;
}

static void
log_request(const int error, struct landlock_request *const request,
	    const struct landlock_ruleset *const domain,
	    const access_mask_t access_request,
	    const layer_mask_t (*const layer_masks)[LANDLOCK_NUM_ACCESS_FS])
{
	struct audit_buffer *ab;

	if (WARN_ON_ONCE(!error))
		return;
	if (WARN_ON_ONCE(!request))
		return;
	if (WARN_ON_ONCE(!domain || !domain->hierarchy))
		return;

	/* Uses GFP_ATOMIC to not sleep. */
	ab = audit_log_start(audit_context(), GFP_ATOMIC | __GFP_NOWARN,
			     AUDIT_LANDLOCK);
	if (!ab)
		return;

	update_request(request, domain, access_request, layer_masks);

	log_task(ab);
	audit_log_format(ab, " domain=%llu op=%s errno=%d missing-fs-accesses=",
			 request->youngest_domain,
			 op_to_string(request->operation), -error);
	log_accesses(ab, request->missing_access);
	audit_log_format(ab, " missing-permission=%s",
			 perm_to_string(request->missing_permission));
	audit_log_lsm_data(ab, &request->audit);
	audit_log_end(ab);
}

// TODO: Make it generic, not FS-centric.
int landlock_log_request(
	const int error, struct landlock_request *const request,
	const struct landlock_ruleset *const domain,
	const access_mask_t access_request,
	const layer_mask_t (*const layer_masks)[LANDLOCK_NUM_ACCESS_FS])
{
	/* No need to log the access request, only the missing accesses. */
	log_request(error, request, domain, access_request, layer_masks);
	return error;
}
