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
