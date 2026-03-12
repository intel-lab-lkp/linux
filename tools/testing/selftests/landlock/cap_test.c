// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Capability restriction
 *
 * Copyright © 2026 Cloudflare
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/landlock.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audit.h"
#include "common.h"

static int create_cap_ruleset(void)
{
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_CAPABILITY_USE,
	};

	return landlock_create_ruleset(&attr, sizeof(attr), 0);
}

static int add_cap_rule(int ruleset_fd, __u64 cap)
{
	const struct landlock_capability_attr attr = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << cap),
	};

	return landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY, &attr,
				 0);
}

TEST(add_rule_bad_attr)
{
	const struct landlock_ruleset_attr ns_only_attr = {
		.handled_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
	};
	int ruleset_fd;
	struct landlock_capability_attr attr = {};

	ruleset_fd = create_cap_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/* Empty allowed_perm returns ENOMSG (useless deny rule). */
	attr.allowed_perm = 0;
	attr.capabilities = (1ULL << CAP_NET_RAW);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 0));
	ASSERT_EQ(ENOMSG, errno);

	/* Useless rule: empty capabilities bitmask. */
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE;
	attr.capabilities = 0;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 0));
	ASSERT_EQ(ENOMSG, errno);

	/* allowed_perm with unhandled bit. */
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE |
			    LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.capabilities = (1ULL << CAP_NET_RAW);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);

	/* allowed_perm with wrong type. */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.capabilities = (1ULL << CAP_NET_RAW);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);

	/*
	 * Unknown capability bits (e.g. bit 63) are silently accepted
	 * for forward compatibility.  Only known bits are stored.
	 */
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE;
	attr.capabilities = 1ULL << 63;
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &attr, 0));

	/* Non-zero flags must be rejected. */
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE;
	attr.capabilities = (1ULL << CAP_NET_RAW);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 1));
	ASSERT_EQ(EINVAL, errno);

	EXPECT_EQ(0, close(ruleset_fd));

	/*
	 * Ruleset handles PERM_NAMESPACE_ENTER but not PERM_CAPABILITY_USE:
	 * adding a capability rule must be rejected.
	 */
	ruleset_fd =
		landlock_create_ruleset(&ns_only_attr, sizeof(ns_only_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE;
	attr.capabilities = (1ULL << CAP_NET_RAW);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);
	EXPECT_EQ(0, close(ruleset_fd));
}

/*
 * Unknown capability values above CAP_LAST_CAP are silently accepted
 * (allow-list: they have no effect since the kernel never checks them).
 */
TEST(add_rule_unknown)
{
	int ruleset_fd;
	struct landlock_capability_attr attr = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
	};

	ruleset_fd = create_cap_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/* Just above CAP_LAST_CAP should succeed. */
	attr.capabilities = (1ULL << (CAP_LAST_CAP + 1));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &attr, 0));

	/* High values (below bit 63) should succeed. */
	attr.capabilities = (1ULL << 62);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &attr, 0));

	EXPECT_EQ(0, close(ruleset_fd));
}

/* clang-format off */
FIXTURE(cap_enforce) {};
/* clang-format on */

FIXTURE_VARIANT(cap_enforce)
{
	const bool is_sandboxed;
	const bool handle_caps;
	const __u64 allowed_cap;
	const int expected_sysadmin;
	const int expected_chroot;
};

/*
 * Unsandboxed baseline: no Landlock domain is enforced.
 * Both capabilities should work normally.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_enforce, unsandboxed) {
	/* clang-format on */
	.is_sandboxed = false,	.handle_caps = false, .allowed_cap = 0,
	.expected_sysadmin = 0, .expected_chroot = 0,
};

/*
 * Denied: capabilities are handled but no rule allows them.
 * All capability checks must be denied by Landlock even if the
 * capability is effective.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_enforce, denied) {
	/* clang-format on */
	.is_sandboxed = true,	    .handle_caps = true,      .allowed_cap = 0,
	.expected_sysadmin = EPERM, .expected_chroot = EPERM,
};

/*
 * Allowed: CAP_SYS_ADMIN is allowed by rule, CAP_SYS_CHROOT is not.
 * Only the explicitly allowed capability should succeed.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_enforce, allowed) {
	/* clang-format on */
	.is_sandboxed = true,	      .handle_caps = true,
	.allowed_cap = CAP_SYS_ADMIN, .expected_sysadmin = 0,
	.expected_chroot = EPERM,
};

/*
 * Unhandled: the ruleset does not handle LANDLOCK_PERM_CAPABILITY_USE
 * at all (only handles FS access).  Both capabilities should work
 * since the domain does not restrict them.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_enforce, unhandled) {
	/* clang-format on */
	.is_sandboxed = true,	.handle_caps = false, .allowed_cap = 0,
	.expected_sysadmin = 0, .expected_chroot = 0,
};

FIXTURE_SETUP(cap_enforce)
{
	disable_caps(_metadata);
}

FIXTURE_TEARDOWN(cap_enforce)
{
}

/*
 * Capability enforcement: tests the four fundamental enforcement
 * scenarios (unsandboxed baseline, denied, allowed, unhandled) using
 * two independent capability checks (sethostname for CAP_SYS_ADMIN,
 * chroot for CAP_SYS_CHROOT).
 */
TEST_F(cap_enforce, use)
{
	int ruleset_fd;

	/* Isolate hostname changes from other tests. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, unshare(CLONE_NEWUTS));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	if (variant->is_sandboxed) {
		if (variant->handle_caps) {
			ruleset_fd = create_cap_ruleset();
		} else {
			const struct landlock_ruleset_attr attr = {
				.handled_access_fs =
					LANDLOCK_ACCESS_FS_READ_FILE,
			};

			ruleset_fd =
				landlock_create_ruleset(&attr, sizeof(attr), 0);
		}
		ASSERT_LE(0, ruleset_fd);

		if (variant->allowed_cap)
			ASSERT_EQ(0, add_cap_rule(ruleset_fd,
						  variant->allowed_cap));

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	/* Test CAP_SYS_ADMIN via sethostname. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	if (variant->expected_sysadmin) {
		EXPECT_EQ(-1, sethostname("test", 4));
		EXPECT_EQ(variant->expected_sysadmin, errno);
	} else {
		EXPECT_EQ(0, sethostname("test", 4));
	}
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Test CAP_SYS_CHROOT via chroot. */
	set_cap(_metadata, CAP_SYS_CHROOT);
	if (variant->expected_chroot) {
		EXPECT_EQ(-1, chroot("/"));
		EXPECT_EQ(variant->expected_chroot, errno);
	} else {
		EXPECT_EQ(0, chroot("/"));
	}
}

/*
 * Layer stacking: layer 1 always allows CAP_SYS_ADMIN.  Layer 2
 * either allows (both layers agree -> success) or denies (any layer
 * can deny -> failure).
 */
/* clang-format off */
FIXTURE(cap_stacking) {};
/* clang-format on */

FIXTURE_VARIANT(cap_stacking)
{
	const bool is_sandboxed;
	const bool second_layer_allows;
	const bool second_layer_is_fs_only;
	const int expected_sysadmin;
	const int expected_chroot;
};

/*
 * Unsandboxed baseline: no Landlock layers are stacked.
 * Both capabilities should work normally.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_stacking, unsandboxed) {
	/* clang-format on */
	.is_sandboxed = false,
	.second_layer_allows = false,
	.expected_sysadmin = 0,
	.expected_chroot = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(cap_stacking, deny) {
	/* clang-format on */
	.is_sandboxed = true,
	.second_layer_allows = false,
	.expected_sysadmin = EPERM,
	.expected_chroot = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(cap_stacking, allow) {
	/* clang-format on */
	.is_sandboxed = true,
	.second_layer_allows = true,
	.expected_sysadmin = 0,
	.expected_chroot = EPERM,
};

/*
 * Mixed layers: first layer handles PERM_CAPABILITY_USE (denies all
 * caps), second layer is FS-only (does not handle it).  The perm
 * walker iterates from youngest (layer 1) to oldest (layer 0) and
 * must skip the FS-only layer to find the denying layer beneath.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(cap_stacking, mixed_layers) {
	/* clang-format on */
	.is_sandboxed = true,
	.second_layer_is_fs_only = true,
	.expected_sysadmin = EPERM,
	.expected_chroot = EPERM,
};

FIXTURE_SETUP(cap_stacking)
{
	disable_caps(_metadata);
}

FIXTURE_TEARDOWN(cap_stacking)
{
}

TEST_F(cap_stacking, two_layers)
{
	int ruleset_fd;

	if (variant->is_sandboxed) {
		/* First layer: always handles PERM_CAPABILITY_USE. */
		ruleset_fd = create_cap_ruleset();
		ASSERT_LE(0, ruleset_fd);
		if (!variant->second_layer_is_fs_only)
			ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_SYS_ADMIN));

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));

		if (variant->second_layer_is_fs_only) {
			/*
			 * Second layer: FS-only (does not handle
			 * PERM_CAPABILITY_USE).  The perm walker must
			 * skip this layer.
			 */
			const struct landlock_ruleset_attr fs_attr = {
				.handled_access_fs =
					LANDLOCK_ACCESS_FS_READ_FILE,
			};

			ruleset_fd = landlock_create_ruleset(
				&fs_attr, sizeof(fs_attr), 0);
		} else {
			/* Second layer: cap allow or deny. */
			ruleset_fd = create_cap_ruleset();
			if (variant->second_layer_allows)
				ASSERT_EQ(0, add_cap_rule(ruleset_fd,
							  CAP_SYS_ADMIN));
		}
		ASSERT_LE(0, ruleset_fd);
		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	/* Test CAP_SYS_ADMIN via sethostname. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	if (variant->expected_sysadmin) {
		EXPECT_EQ(-1, sethostname("test", 4));
		EXPECT_EQ(variant->expected_sysadmin, errno);
	} else {
		EXPECT_EQ(0, sethostname("test", 4));
	}
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Test CAP_SYS_CHROOT via chroot. */
	set_cap(_metadata, CAP_SYS_CHROOT);
	if (variant->expected_chroot) {
		EXPECT_EQ(-1, chroot("/"));
		EXPECT_EQ(variant->expected_chroot, errno);
	} else {
		EXPECT_EQ(0, chroot("/"));
	}
	clear_cap(_metadata, CAP_SYS_CHROOT);
}

/*
 * Verify that LANDLOCK_PERM_CAPABILITY_USE enforces when the domain is applied
 * without no_new_privs, using CAP_SYS_ADMIN for landlock_restrict_self()
 * authorization instead.  Privileged processes (e.g. container managers)
 * can sandbox themselves this way.
 */
TEST(cap_without_nnp)
{
	int ruleset_fd;

	disable_caps(_metadata);

	ruleset_fd = create_cap_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/* Allow CAP_SYS_CHROOT but not CAP_SYS_ADMIN. */
	ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_SYS_CHROOT));

	/*
	 * Enforce WITHOUT NNP: landlock_restrict_self() succeeds when
	 * the caller has CAP_SYS_ADMIN (checked before the new domain
	 * takes effect).
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, landlock_restrict_self(ruleset_fd, 0));
	EXPECT_EQ(0, close(ruleset_fd));

	/*
	 * CAP_SYS_ADMIN is still in effective set but Landlock denies it:
	 * cap_capable() returns 0, then hook_capable() returns -EPERM.
	 */
	EXPECT_EQ(-1, sethostname("test", 4));
	EXPECT_EQ(EPERM, errno);

	/* CAP_SYS_CHROOT is allowed by the rule. */
	set_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(0, chroot("/"));
}

/*
 * Verify that capabilities gained through user namespace ownership are
 * still restricted by LANDLOCK_PERM_CAPABILITY_USE.  When a process creates a
 * user namespace, the kernel grants CAP_FULL_SET in the new namespace
 * via cap_capable_helper()'s ownership bypass.  Landlock's hook_capable()
 * must still deny capabilities not in the allowed set, ensuring that
 * user namespace creation cannot be used to escape capability restrictions.
 */
TEST(cap_userns_ownership_bypass)
{
	pid_t child;
	int status;

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		int ruleset_fd;

		disable_caps(_metadata);

		ruleset_fd = create_cap_ruleset();
		ASSERT_LE(0, ruleset_fd);

		/* Allow CAP_SYS_ADMIN only. */
		ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_SYS_ADMIN));
		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));

		/*
		 * Create a user namespace.  This is unprivileged and
		 * does not require capabilities.  LANDLOCK_PERM_NAMESPACE_ENTER
		 * is not handled so namespace creation is unrestricted.
		 */
		ASSERT_EQ(0, unshare(CLONE_NEWUSER));

		/*
		 * After unshare(CLONE_NEWUSER), the kernel set
		 * cap_effective = CAP_FULL_SET in the new namespace.
		 * Create a UTS namespace (requires CAP_SYS_ADMIN in
		 * the new user NS).  Landlock allows CAP_SYS_ADMIN.
		 */
		ASSERT_EQ(0, unshare(CLONE_NEWUTS))
		{
			TH_LOG("unshare(CLONE_NEWUTS): %s", strerror(errno));
		}

		/*
		 * sethostname checks against uts_ns->user_ns, which is
		 * now the new user NS.  CAP_SYS_ADMIN is allowed.
		 */
		EXPECT_EQ(0, sethostname("test", 4));

		/*
		 * chroot checks against current_user_ns(), which is
		 * the new user NS.  The process has CAP_SYS_CHROOT in
		 * cap_effective (from user NS creation), so cap_capable()
		 * returns 0.  But Landlock denies because no rule
		 * allows CAP_SYS_CHROOT.
		 */
		EXPECT_EQ(-1, chroot("/"));
		EXPECT_EQ(EPERM, errno);

		_exit(_metadata->exit_code);
		return;
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (WIFSIGNALED(status) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != EXIT_SUCCESS)
		_metadata->exit_code = KSFT_FAIL;
}

/* Audit tests */

static int matches_log_cap(int audit_fd, int cap_number)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=perm\\.capability_use capability=%d $";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len = snprintf(log_match, sizeof(log_match), log_template,
				 cap_number);
	if (log_match_len >= sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_ACCESS, log_match,
				  NULL);
}

FIXTURE(cap_audit)
{
	struct audit_filter audit_filter;
	int audit_fd;
};

FIXTURE_SETUP(cap_audit)
{
	ASSERT_TRUE(is_in_init_user_ns());

	disable_caps(_metadata);

	set_cap(_metadata, CAP_AUDIT_CONTROL);
	self->audit_fd = audit_init_with_exe_filter(&self->audit_filter);
	EXPECT_LE(0, self->audit_fd);
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

FIXTURE_TEARDOWN(cap_audit)
{
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, audit_cleanup(self->audit_fd, &self->audit_filter));
}

/*
 * Verifies that a denied capability produces the expected audit record
 * with the correct capability number and blocker string.
 */
TEST_F(cap_audit, denied)
{
	struct audit_records records;
	int ruleset_fd;

	/* Baseline: chroot works before Landlock. */
	set_cap(_metadata, CAP_SYS_CHROOT);
	ASSERT_EQ(0, chroot("/"));
	clear_cap(_metadata, CAP_SYS_CHROOT);

	ruleset_fd = create_cap_ruleset();
	ASSERT_LE(0, ruleset_fd);
	/* Allow CAP_AUDIT_CONTROL for child-side audit cleanup. */
	ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_AUDIT_CONTROL));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* Deny CAP_SYS_CHROOT (no allow rule). */
	set_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(-1, chroot("/"));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_CHROOT);

	EXPECT_EQ(0, matches_log_cap(self->audit_fd, CAP_SYS_CHROOT));

	/*
	 * No extra access records: the denial was already consumed by
	 * matches_log_cap above.  One domain allocation record, emitted
	 * in the same event as the first access denial for this domain.
	 */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(cap_audit, allowed)
{
	struct audit_records records;
	int ruleset_fd;

	ruleset_fd = create_cap_ruleset();
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_SYS_ADMIN));
	/* Allow CAP_AUDIT_CONTROL for child-side audit cleanup. */
	ASSERT_EQ(0, add_cap_rule(ruleset_fd, CAP_AUDIT_CONTROL));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, sethostname("test", 4));

	/* No records: allowed operations never trigger audit logging. */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
}

TEST_HARNESS_MAIN
