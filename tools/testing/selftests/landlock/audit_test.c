// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Audit
 *
 * Copyright © 2024 Microsoft Corporation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <linux/landlock.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audit.h"
#include "common.h"

static int matches_log_dom_info(struct __test_metadata *const _metadata,
				int audit_fd)
{
	return audit_match_record(
		audit_fd, AUDIT_LANDLOCK_DOM_INFO,
		REGEX_LANDLOCK_PREFIX
		" creation=[0-9.]\\+ pid=[0-9]\\+ uid=[0-9]\\+ exe=\"[^\"]\\+\" comm=\"audit_test\"$");
}

static int matches_log_umount(struct __test_metadata *const _metadata,
			      int audit_fd)
{
	return audit_match_record(audit_fd, AUDIT_LANDLOCK_DENY,
				  REGEX_LANDLOCK_PREFIX " blockers=.*");
}

FIXTURE(audit)
{
	struct audit_filter audit_filter;
	int audit_fd;
};

FIXTURE_VARIANT(audit)
{
	const int restrict_flags;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit, default) {};
/* clang-format on */

/* clang-format off */
FIXTURE_VARIANT_ADD(audit, quiet) {
	/* clang-format on */
	.restrict_flags = LANDLOCK_RESTRICT_SELF_QUIET,
};

FIXTURE_SETUP(audit)
{
	disable_caps(_metadata);
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	self->audit_fd = audit_init_with_exe_filter(&self->audit_filter);
	EXPECT_LE(0, self->audit_fd)
	{
		const char *error_msg;

		/* kill "$(auditctl -s | sed -ne 's/^pid \([0-9]\+\)$/\1/p')" */
		if (self->audit_fd == -EEXIST)
			error_msg = "socket already in use (e.g. auditd)";
		else
			error_msg = strerror(-self->audit_fd);
		TH_LOG("Failed to initialize audit: %s", error_msg);
	}
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

FIXTURE_TEARDOWN(audit)
{
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, audit_cleanup(self->audit_fd, &self->audit_filter));
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

TEST_F(audit, fs_deny)
{
	int status;
	pid_t child;
	struct audit_records records;

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		const struct landlock_ruleset_attr ruleset_attr = {
			.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE,
		};
		int ruleset_fd;

		/* Add filesystem restrictions. */
		ruleset_fd = landlock_create_ruleset(&ruleset_attr,
						     sizeof(ruleset_attr), 0);
		ASSERT_LE(0, ruleset_fd);
		EXPECT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
		ASSERT_EQ(0, landlock_restrict_self(ruleset_fd,
						    variant->restrict_flags));
		EXPECT_EQ(0, close(ruleset_fd));

		/* First umount checks to test log entries. */
		set_cap(_metadata, CAP_SYS_ADMIN);
		EXPECT_EQ(-1, umount("/"));
		EXPECT_EQ(EPERM, errno);
		clear_cap(_metadata, CAP_SYS_ADMIN);

		if (variant->restrict_flags & LANDLOCK_RESTRICT_SELF_QUIET) {
			EXPECT_EQ(-EAGAIN, matches_log_umount(_metadata,
							      self->audit_fd));
		} else {
			EXPECT_EQ(0, matches_log_umount(_metadata,
							self->audit_fd));

			/* Checks domain information records. */
			EXPECT_EQ(0, matches_log_dom_info(_metadata,
							  self->audit_fd));
		}

		/* Second umount checks to test audit_count_records(). */
		set_cap(_metadata, CAP_SYS_ADMIN);
		EXPECT_EQ(-1, umount("/"));
		EXPECT_EQ(EPERM, errno);
		clear_cap(_metadata, CAP_SYS_ADMIN);

		/* Makes sure there is no superfluous logged records. */
		audit_count_records(self->audit_fd, &records);
		if (variant->restrict_flags & LANDLOCK_RESTRICT_SELF_QUIET) {
			EXPECT_EQ(0, records.deny);
		} else {
			EXPECT_EQ(1, records.deny);
		}
		EXPECT_EQ(0, records.info);
		EXPECT_EQ(0, records.drop);

		/* Updates filter rules to match the drop record. */
		set_cap(_metadata, CAP_AUDIT_CONTROL);
		EXPECT_EQ(0, audit_filter_drop(self->audit_fd, AUDIT_ADD_RULE));
		EXPECT_EQ(0, audit_filter_exe(
				     self->audit_fd, &self->audit_filter,
				     AUDIT_DEL_RULE, AUDIT_FILTER_EXCLUDE));
		clear_cap(_metadata, CAP_AUDIT_CONTROL);

		_exit(_metadata->exit_code);
		return;
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

TEST_HARNESS_MAIN
