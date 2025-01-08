// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Audit
 *
 * Copyright © 2024 Microsoft Corporation
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/landlock.h>
#include <stdlib.h>
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

static int matches_log_signal(struct __test_metadata *const _metadata,
			      int audit_fd, const pid_t opid)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=scope.signal opid=%d ocomm=\"audit_test\"$";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len =
		snprintf(log_match, sizeof(log_match), log_template, opid);
	if (log_match_len > sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_DENY, log_match);
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

FIXTURE(audit_rule)
{
	struct audit_filter audit_filter_main, audit_filter_test;
	int audit_fd;
};

FIXTURE_VARIANT(audit_rule)
{
	const bool with_exe_landlock_deny_child;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_rule, exe_landlock_deny_child) {
	/* clang-format on */
	.with_exe_landlock_deny_child = true,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_rule, exe_landlock_deny_parent) {
	/* clang-format on */
	.with_exe_landlock_deny_child = false,
};

FIXTURE_SETUP(audit_rule)
{
	const char *path = NULL;

	disable_caps(_metadata);
	set_cap(_metadata, CAP_AUDIT_CONTROL);

	if (variant->with_exe_landlock_deny_child)
		/* Filter on the sandboxer instead of the current exe. */
		path = bin_wait_pipe;

	self->audit_fd = audit_init();
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

	/* Applies main filter for the test task. */
	EXPECT_EQ(0, audit_init_filter_exe(AUDIT_EXE, &self->audit_filter_main,
					   bin_wait_pipe));
	EXPECT_EQ(0, audit_filter_exe(self->audit_fd, &self->audit_filter_main,
				      AUDIT_ADD_RULE, AUDIT_FILTER_EXCLUDE));

	/* Applies test filter for the test task or the current task. */
	EXPECT_EQ(0, audit_init_filter_exe(AUDIT_EXE_LANDLOCK_DENY,
					   &self->audit_filter_test, path));
	EXPECT_EQ(0, audit_filter_exe(self->audit_fd, &self->audit_filter_test,
				      AUDIT_ADD_RULE, AUDIT_FILTER_EXCLUDE));

	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

FIXTURE_TEARDOWN(audit_rule)
{
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, audit_filter_exe(self->audit_fd, &self->audit_filter_main,
				      AUDIT_DEL_RULE, AUDIT_FILTER_EXCLUDE));
	EXPECT_EQ(0, audit_filter_exe(self->audit_fd, &self->audit_filter_test,
				      AUDIT_DEL_RULE, AUDIT_FILTER_EXCLUDE));
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, close(self->audit_fd));
}

TEST_F(audit_rule, exe_landlock_deny)
{
	struct audit_records records;
	int pipe_child[2], pipe_parent[2];
	char buf_parent;
	pid_t child;
	int status;

	ASSERT_EQ(0, pipe2(pipe_child, 0));
	ASSERT_EQ(0, pipe2(pipe_parent, 0));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		char pipe_child_str[12], pipe_parent_str[12];
		char *const argv[] = { (char *)bin_wait_pipe, pipe_child_str,
				       pipe_parent_str, NULL };

		/* Passes the pipe FDs to the executed binary. */
		EXPECT_EQ(0, close(pipe_child[0]));
		EXPECT_EQ(0, close(pipe_parent[1]));
		snprintf(pipe_child_str, sizeof(pipe_child_str), "%d",
			 pipe_child[1]);
		snprintf(pipe_parent_str, sizeof(pipe_parent_str), "%d",
			 pipe_parent[0]);

		ASSERT_EQ(0, execve(argv[0], argv, NULL))
		{
			TH_LOG("Failed to execute \"%s\": %s", argv[0],
			       strerror(errno));
		};
		_exit(1);
		return;
	}

	EXPECT_EQ(0, close(pipe_child[1]));
	EXPECT_EQ(0, close(pipe_parent[0]));

	/* Waits for the child. */
	EXPECT_EQ(1, read(pipe_child[0], &buf_parent, 1));

	/* Tests that there was no denial until now. */
	audit_count_records(self->audit_fd, &records);
	EXPECT_EQ(0, records.deny);

	/* Signals the child to terminate. */
	EXPECT_EQ(1, write(pipe_parent[1], ".", 1));

	/* Tests that the audit record only matches the child. */
	if (variant->with_exe_landlock_deny_child) {
		EXPECT_EQ(0, matches_log_signal(_metadata, self->audit_fd,
						getpid()));
	} else {
		audit_count_records(self->audit_fd, &records);
		EXPECT_EQ(0, records.deny);
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
}

TEST_HARNESS_MAIN
