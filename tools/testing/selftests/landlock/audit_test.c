// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Audit
 *
 * Copyright © 2024-2025 Microsoft Corporation
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

static int matches_log_domain_allocated(struct __test_metadata *const _metadata,
					int audit_fd)
{
	return audit_match_record(
		audit_fd, AUDIT_LANDLOCK_DOMAIN,
		REGEX_LANDLOCK_PREFIX
		" status=allocated mode=enforcing pid=[0-9]\\+ uid=[0-9]\\+"
		" exe=\"[^\"]\\+\" comm=\"audit_test\"$");
}

static int
matches_log_domain_deallocated(struct __test_metadata *const _metadata,
			       int audit_fd, unsigned int num_denials)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" status=deallocated denials=%u$";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len = snprintf(log_match, sizeof(log_match), log_template,
				 num_denials);
	if (log_match_len > sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_DOMAIN, log_match);
}

static int matches_log_signal(struct __test_metadata *const _metadata,
			      int audit_fd, const pid_t opid)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=scope\\.signal opid=%d ocomm=\"audit_test\"$";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len =
		snprintf(log_match, sizeof(log_match), log_template, opid);
	if (log_match_len > sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_ACCESS, log_match);
}

FIXTURE(audit_fork)
{
	struct audit_filter audit_filter;
	int audit_fd;
};

FIXTURE_VARIANT(audit_fork)
{
	const int restrict_flags;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_fork, default) {
	/* clang-format on */
	.restrict_flags = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_fork, quiet) {
	/* clang-format on */
	.restrict_flags = LANDLOCK_RESTRICT_SELF_QUIET,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_fork, quiet_subdomains) {
	/* clang-format on */
	.restrict_flags = LANDLOCK_RESTRICT_SELF_QUIET_SUBDOMAINS,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(audit_fork, log_cross_exec) {
	/* clang-format on */
	.restrict_flags = LANDLOCK_RESTRICT_SELF_LOG_CROSS_EXEC,
};

FIXTURE_SETUP(audit_fork)
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

FIXTURE_TEARDOWN(audit_fork)
{
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, audit_cleanup(self->audit_fd, &self->audit_filter));
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

TEST_F(audit_fork, flags)
{
	int status;
	pid_t child;
	struct audit_records records;

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		const struct landlock_ruleset_attr ruleset_attr = {
			.scoped = LANDLOCK_SCOPE_SIGNAL,
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

		/* First signal checks to test log entries. */
		EXPECT_EQ(-1, kill(getppid(), 0));
		EXPECT_EQ(EPERM, errno);

		if (variant->restrict_flags & LANDLOCK_RESTRICT_SELF_QUIET) {
			EXPECT_EQ(-EAGAIN,
				  matches_log_signal(_metadata, self->audit_fd,
						     getppid()));
		} else {
			EXPECT_EQ(0,
				  matches_log_signal(_metadata, self->audit_fd,
						     getppid()));

			/* Checks domain information records. */
			EXPECT_EQ(0, matches_log_domain_allocated(
					     _metadata, self->audit_fd));
		}

		/* Second signal checks to test audit_count_records(). */
		EXPECT_EQ(-1, kill(getppid(), 0));
		EXPECT_EQ(EPERM, errno);

		/* Makes sure there is no superfluous logged records. */
		audit_count_records(self->audit_fd, &records);
		if (variant->restrict_flags & LANDLOCK_RESTRICT_SELF_QUIET) {
			EXPECT_EQ(0, records.access);
		} else {
			EXPECT_EQ(1, records.access);
		}
		EXPECT_EQ(0, records.domain);

		/* Updates filter rules to match the drop record. */
		set_cap(_metadata, CAP_AUDIT_CONTROL);
		EXPECT_EQ(0, audit_filter_drop(self->audit_fd, AUDIT_ADD_RULE));
		EXPECT_EQ(0,
			  audit_filter_exe(self->audit_fd, &self->audit_filter,
					   AUDIT_DEL_RULE));
		clear_cap(_metadata, CAP_AUDIT_CONTROL);

		_exit(_metadata->exit_code);
		return;
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;

	if (variant->restrict_flags & LANDLOCK_RESTRICT_SELF_QUIET) {
		EXPECT_EQ(-EAGAIN, matches_log_domain_deallocated(
					   _metadata, self->audit_fd, 0));
	} else {
		EXPECT_EQ(0, matches_log_domain_deallocated(_metadata,
							    self->audit_fd, 2));
	}
}

TEST_HARNESS_MAIN
