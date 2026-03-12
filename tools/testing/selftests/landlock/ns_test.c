// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Namespace restriction
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
#include <sys/prctl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <unistd.h>

#include "audit.h"
#include "common.h"

/*
 * Max length for /proc/self/ns/<name> paths (longest:
 * "/proc/self/ns/cgroup").
 */
#define NS_PROC_PATH_MAX 32

static int create_ns_ruleset(void)
{
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
	};

	return landlock_create_ruleset(&attr, sizeof(attr), 0);
}

static int add_ns_rule(int ruleset_fd, __u64 ns_type)
{
	const struct landlock_namespace_attr attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
		.namespace_types = ns_type,
	};

	return landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE, &attr, 0);
}

/*
 * Returns the /proc/self/NS entry name for a given CLONE_NEW* type, or NULL
 * if unknown.  Used to check kernel support without side effects.
 */
static const char *ns_proc_name(__u64 ns_type)
{
	switch (ns_type) {
	case CLONE_NEWNS:
		return "mnt";
	case CLONE_NEWCGROUP:
		return "cgroup";
	case CLONE_NEWUTS:
		return "uts";
	case CLONE_NEWIPC:
		return "ipc";
	case CLONE_NEWUSER:
		return "user";
	case CLONE_NEWPID:
		return "pid";
	case CLONE_NEWNET:
		return "net";
	case CLONE_NEWTIME:
		return "time";
	default:
		return NULL;
	}
}

static bool ns_is_supported(__u64 ns_type, char *proc_path, size_t size)
{
	const char *ns_name;

	ns_name = ns_proc_name(ns_type);
	if (!ns_name)
		return false;

	snprintf(proc_path, size, "/proc/self/ns/%s", ns_name);
	return access(proc_path, F_OK) == 0;
}

/* Rule validation tests */

TEST(add_rule_bad_attr)
{
	const struct landlock_ruleset_attr cap_only_attr = {
		.handled_perm = LANDLOCK_PERM_CAPABILITY_USE,
	};
	int ruleset_fd;
	struct landlock_namespace_attr attr = {};

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/* Empty allowed_perm returns ENOMSG (useless deny rule). */
	attr.allowed_perm = 0;
	attr.namespace_types = CLONE_NEWUTS;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 0));
	ASSERT_EQ(ENOMSG, errno);

	/* allowed_perm with unhandled bit. */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER |
			    LANDLOCK_PERM_CAPABILITY_USE;
	attr.namespace_types = CLONE_NEWUTS;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);

	/* allowed_perm with wrong type. */
	attr.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE;
	attr.namespace_types = CLONE_NEWUTS;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);

	/*
	 * Unknown namespace bits (e.g. bit 63) are silently accepted
	 * for forward compatibility.  Only known CLONE_NEW* bits are stored.
	 */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = 1ULL << 63;
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &attr, 0));

	/* Useless rule: empty namespace_types bitmask. */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = 0;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 0));
	ASSERT_EQ(ENOMSG, errno);

	/*
	 * Bit 1 is not a CLONE_NEW* value but is silently accepted
	 * for forward compatibility (no hole rejection).
	 */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = (1ULL << 1);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &attr, 0));

	/* Multi-bit values are valid (bitmask allows multiple types). */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = CLONE_NEWUTS | CLONE_NEWNET;
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &attr, 0));

	/* Non-zero flags must be rejected. */
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = CLONE_NEWUTS;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 1));
	ASSERT_EQ(EINVAL, errno);

	EXPECT_EQ(0, close(ruleset_fd));

	/*
	 * Ruleset handles PERM_CAPABILITY_USE but not PERM_NAMESPACE_ENTER:
	 * adding a namespace rule must be rejected.
	 */
	ruleset_fd = landlock_create_ruleset(&cap_only_attr,
					     sizeof(cap_only_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	attr.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER;
	attr.namespace_types = CLONE_NEWUTS;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
					&attr, 0));
	ASSERT_EQ(EINVAL, errno);
	EXPECT_EQ(0, close(ruleset_fd));
}

/*
 * Unknown namespace types in the upper range are silently accepted
 * (allow-list: they have no effect since the kernel never checks them).
 */
TEST(add_rule_unknown)
{
	int ruleset_fd;
	struct landlock_namespace_attr attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
	};

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/*
	 * Bit 31 is in the lower 32 bits but not a CLONE_NEW* value.
	 * Silently accepted for forward compatibility (no hole rejection).
	 */
	attr.namespace_types = 1ULL << 31;
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &attr, 0));

	/* Bit 32 is in the unknown upper range: silently accepted. */
	attr.namespace_types = 1ULL << 32;
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &attr, 0));

	EXPECT_EQ(0, close(ruleset_fd));
}

/* Namespace creation tests (variant-based positive/negative) */

/* clang-format off */
FIXTURE(ns_create) {
	char proc_path[NS_PROC_PATH_MAX];
};
/* clang-format on */

FIXTURE_VARIANT(ns_create)
{
	const __u64 namespace_types;
	const bool is_sandboxed;
	const bool has_rule;
	const bool drop_all_caps;
	const int expected_result;
};

/*
 * Unsandboxed baseline: no Landlock domain is enforced.
 * User namespace creation should succeed without any restriction.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, user_unsandboxed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUSER,
	.is_sandboxed = false,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = 0,
};

/*
 * User namespace creation denied: handled by Landlock but no rule
 * allows CLONE_NEWUSER.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, user_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUSER,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/*
 * User namespace creation allowed: Landlock rule permits CLONE_NEWUSER.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, user_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUSER,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = false,
	.expected_result = 0,
};

/*
 * User namespace creation while unprivileged: the process has no
 * capabilities but unshare(CLONE_NEWUSER) is an unprivileged
 * operation so it still succeeds.  The Landlock rule allows it.
 * For setns, the capability check (CAP_SYS_ADMIN) fails first
 * since the process has no capabilities, yielding EPERM.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, user_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUSER,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = 0,
};

/*
 * Unsandboxed baseline for non-user namespace: no Landlock domain,
 * process has CAP_SYS_ADMIN.  UTS creation should succeed.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, uts_unsandboxed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUTS,
	.is_sandboxed = false,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = 0,
};

/*
 * Non-user namespace denied: process has CAP_SYS_ADMIN (passes
 * ns_capable), but Landlock denies (no rule).
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, uts_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUTS,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/*
 * Non-user namespace allowed: process has CAP_SYS_ADMIN and Landlock
 * rule permits CLONE_NEWUTS.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, uts_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUTS, .is_sandboxed = true, .has_rule = true,
	.drop_all_caps = false,		 .expected_result = 0,
};

/*
 * Unprivileged namespace creation: process lacks CAP_SYS_ADMIN, so the
 * kernel denies creation regardless of Landlock rules.  Landlock cannot
 * authorize what the kernel denied (LSM hooks are restriction-only).
 * The rule is present to verify Landlock does not change the error code.
 */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, uts_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWUTS,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, ipc_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWIPC,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, ipc_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWIPC, .is_sandboxed = true, .has_rule = true,
	.drop_all_caps = false,		 .expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, ipc_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWIPC,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, mnt_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNS,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, mnt_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNS, .is_sandboxed = true, .has_rule = true,
	.drop_all_caps = false,		.expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, mnt_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNS,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, cgroup_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWCGROUP,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, cgroup_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWCGROUP,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = false,
	.expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, cgroup_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWCGROUP,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, pid_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWPID,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, pid_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWPID, .is_sandboxed = true, .has_rule = true,
	.drop_all_caps = false,		 .expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, pid_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWPID,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, net_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNET,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, net_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNET, .is_sandboxed = true, .has_rule = true,
	.drop_all_caps = false,		 .expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, net_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWNET,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, time_denied) {
	/* clang-format on */
	.namespace_types = CLONE_NEWTIME,
	.is_sandboxed = true,
	.has_rule = false,
	.drop_all_caps = false,
	.expected_result = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, time_allowed) {
	/* clang-format on */
	.namespace_types = CLONE_NEWTIME,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = false,
	.expected_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_create, time_unprivileged) {
	/* clang-format on */
	.namespace_types = CLONE_NEWTIME,
	.is_sandboxed = true,
	.has_rule = true,
	.drop_all_caps = true,
	.expected_result = EPERM,
};

FIXTURE_SETUP(ns_create)
{
	if (!ns_is_supported(variant->namespace_types, self->proc_path,
			     sizeof(self->proc_path))) {
		/* UML does not support the time namespace. */
		if (variant->namespace_types == CLONE_NEWTIME)
			SKIP(return, "CLONE_NEWTIME not supported");

		ASSERT_TRUE(false)
		{
			TH_LOG("Namespace type 0x%llx not supported",
			       (unsigned long long)variant->namespace_types);
		}
	}

	if (variant->drop_all_caps)
		drop_caps(_metadata);
	else
		disable_caps(_metadata);
}

FIXTURE_TEARDOWN(ns_create)
{
}

TEST_F(ns_create, unshare)
{
	int ruleset_fd, err;

	if (variant->is_sandboxed) {
		ruleset_fd = create_ns_ruleset();
		ASSERT_LE(0, ruleset_fd);

		if (variant->has_rule)
			ASSERT_EQ(0, add_ns_rule(ruleset_fd,
						 variant->namespace_types));

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	/*
	 * Non-user namespaces need CAP_SYS_ADMIN for the privileged path.
	 * User namespaces and unprivileged tests skip this.
	 */
	if (!variant->drop_all_caps &&
	    variant->namespace_types != CLONE_NEWUSER)
		set_cap(_metadata, CAP_SYS_ADMIN);

	err = unshare(variant->namespace_types);
	if (variant->expected_result) {
		EXPECT_EQ(-1, err);
		EXPECT_EQ(variant->expected_result, errno);
	} else {
		EXPECT_EQ(0, err);
	}

	if (!variant->drop_all_caps &&
	    variant->namespace_types != CLONE_NEWUSER)
		clear_cap(_metadata, CAP_SYS_ADMIN);
}

/*
 * clone3 exercises a different kernel entry point than unshare: it goes
 * through kernel_clone() -> copy_process() -> copy_namespaces() ->
 * create_new_namespaces().  Both paths converge at __ns_common_init() ->
 * security_namespace_alloc(), but the entry point and argument handling
 * differ.
 */
TEST_F(ns_create, clone3)
{
	int ruleset_fd, status;
	pid_t pid;
	struct clone_args args = {};

	if (variant->is_sandboxed) {
		ruleset_fd = create_ns_ruleset();
		ASSERT_LE(0, ruleset_fd);

		if (variant->has_rule)
			ASSERT_EQ(0, add_ns_rule(ruleset_fd,
						 variant->namespace_types));

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	if (!variant->drop_all_caps &&
	    variant->namespace_types != CLONE_NEWUSER)
		set_cap(_metadata, CAP_SYS_ADMIN);

	args.flags = variant->namespace_types;
	args.exit_signal = SIGCHLD;
	pid = sys_clone3(&args, sizeof(args));
	if (pid == 0)
		_exit(EXIT_SUCCESS);

	if (variant->expected_result) {
		EXPECT_EQ(-1, pid);
		EXPECT_EQ(variant->expected_result, errno);
	} else {
		EXPECT_LE(0, pid);
		ASSERT_EQ(pid, waitpid(pid, &status, 0));
		ASSERT_EQ(1, WIFEXITED(status));
		ASSERT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
	}

	if (!variant->drop_all_caps &&
	    variant->namespace_types != CLONE_NEWUSER)
		clear_cap(_metadata, CAP_SYS_ADMIN);
}

/*
 * setns exercises the namespace install path: validate_ns() ->
 * security_namespace_install() -> hook_namespace_install().  This is a
 * different LSM hook than creation, so it must be tested separately for
 * each type.
 *
 * Mount namespace setns requires both CAP_SYS_ADMIN and CAP_SYS_CHROOT
 * (checked by mntns_install), so the allowed variant sets both.
 */
TEST_F(ns_create, setns)
{
	int ruleset_fd, ns_fd, err, expected;

	/*
	 * setns into the process's own user NS always returns EINVAL:
	 * userns_install() rejects re-entry before checking capabilities.
	 */
	if (variant->namespace_types == CLONE_NEWUSER) {
		expected = EINVAL;
	} else {
		expected = variant->expected_result;
	}

	/* Open the NS FD before enforcing the domain. */
	ns_fd = open(self->proc_path, O_RDONLY);
	ASSERT_LE(0, ns_fd);

	if (variant->is_sandboxed) {
		ruleset_fd = create_ns_ruleset();
		ASSERT_LE(0, ruleset_fd);

		if (variant->has_rule)
			ASSERT_EQ(0, add_ns_rule(ruleset_fd,
						 variant->namespace_types));

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	if (!variant->drop_all_caps) {
		set_cap(_metadata, CAP_SYS_ADMIN);
		/*
		 * mntns_install() requires CAP_SYS_CHROOT in addition to
		 * CAP_SYS_ADMIN.
		 */
		if (variant->namespace_types == CLONE_NEWNS)
			set_cap(_metadata, CAP_SYS_CHROOT);
	}

	err = setns(ns_fd, variant->namespace_types);
	if (expected) {
		EXPECT_EQ(-1, err);
		EXPECT_EQ(expected, errno);
	} else {
		EXPECT_EQ(0, err);
	}

	if (!variant->drop_all_caps) {
		clear_cap(_metadata, CAP_SYS_ADMIN);
		if (variant->namespace_types == CLONE_NEWNS)
			clear_cap(_metadata, CAP_SYS_CHROOT);
	}

	EXPECT_EQ(0, close(ns_fd));
}

/* Additional namespace creation tests */

/*
 * When LANDLOCK_PERM_NAMESPACE_ENTER is not handled by any domain, namespace
 * creation must produce the same result as without Landlock.  Unlike the
 * unsandboxed variants of ns_create (which have no domain at all), this test
 * verifies that a domain handling only FS access does not interfere with
 * namespace operations.
 */
TEST(ns_create_unhandled)
{
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE,
	};
	int ruleset_fd;

	disable_caps(_metadata);

	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* User namespace creation should still work (unhandled). */
	EXPECT_EQ(0, unshare(CLONE_NEWUSER));
}

/*
 * Layer stacking: layer 1 always allows CLONE_NEWUSER.  Layer 2
 * either allows (both layers agree -> success) or denies (any layer
 * can deny -> failure).
 */
/* clang-format off */
FIXTURE(ns_stacking) {};
/* clang-format on */

FIXTURE_VARIANT(ns_stacking)
{
	bool second_layer_allows;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ns_stacking, deny) {
	/* clang-format on */
	.second_layer_allows = false,
};

/* Both layers allow CLONE_NEWUSER -> operation succeeds. */
/* clang-format off */
FIXTURE_VARIANT_ADD(ns_stacking, allow) {
	/* clang-format on */
	.second_layer_allows = true,
};

FIXTURE_SETUP(ns_stacking)
{
	disable_caps(_metadata);
}

FIXTURE_TEARDOWN(ns_stacking)
{
}

/*
 * Verify that a second Landlock layer cannot override the first layer's
 * denial.  Each layer stores its permission bitmask independently, and
 * enforcement requires all layers to allow an operation.  This ensures
 * the correct intersection: layer 1 allows CLONE_NEWUSER, but if layer
 * 2 does not also allow it, the operation is denied.
 */
TEST_F(ns_stacking, two_layers)
{
	int ruleset_fd;

	/* First layer: allow CLONE_NEWUSER. */
	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, add_ns_rule(ruleset_fd, CLONE_NEWUSER));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* Second layer: allow or deny depending on variant. */
	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	if (variant->second_layer_allows)
		ASSERT_EQ(0, add_ns_rule(ruleset_fd, CLONE_NEWUSER));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	if (variant->second_layer_allows) {
		EXPECT_EQ(0, unshare(CLONE_NEWUSER));
	} else {
		EXPECT_EQ(-1, unshare(CLONE_NEWUSER));
		EXPECT_EQ(EPERM, errno);
	}
}

/*
 * Combined capability and namespace permissions in a single domain.
 * Verifies that both permission types can coexist and are enforced
 * independently.
 */
TEST(combined_cap_ns)
{
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_CAPABILITY_USE |
				LANDLOCK_PERM_NAMESPACE_ENTER,
	};
	const struct landlock_capability_attr cap_attr = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << CAP_SYS_ADMIN),
	};
	const struct landlock_namespace_attr ns_attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
		.namespace_types = CLONE_NEWUSER,
	};
	int ruleset_fd;

	/* Isolate hostname changes from other tests. */
	ASSERT_EQ(0, unshare(CLONE_NEWUTS));

	disable_caps(_metadata);

	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &cap_attr, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &ns_attr, 0));

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* CAP_SYS_ADMIN use allowed by capability rule. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, sethostname("test", 4));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* CAP_SYS_CHROOT denied (not in allowed capability rules). */
	set_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(-1, chroot("/"));
	EXPECT_EQ(EPERM, errno);

	/*
	 * UTS namespace creation denied by Landlock (not in allowed namespace
	 * rules).  CAP_SYS_ADMIN is needed for the kernel's ns_capable()
	 * check to pass, so that Landlock's hook is actually reached.
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, unshare(CLONE_NEWUTS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* User namespace creation allowed by namespace rule. */
	EXPECT_EQ(0, unshare(CLONE_NEWUSER));
}

/*
 * Partial allow: one namespace type is allowed, another is denied.
 * Verifies that rules are per-type.
 */
TEST(ns_create_partial)
{
	int ruleset_fd;

	disable_caps(_metadata);

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);

	/* Only allow UTS namespace creation. */
	ASSERT_EQ(0, add_ns_rule(ruleset_fd, CLONE_NEWUTS));

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* UTS namespace should be allowed. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, unshare(CLONE_NEWUTS));

	/* User namespace should be denied (no rule). */
	EXPECT_EQ(-1, unshare(CLONE_NEWUSER));
	EXPECT_EQ(EPERM, errno);
}

/* clang-format off */
FIXTURE(setns_cross_process) {};
/* clang-format on */

FIXTURE_VARIANT(setns_cross_process)
{
	bool is_sandboxed;
	int expected_setns;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(setns_cross_process, denied) {
	/* clang-format on */
	.is_sandboxed = true,
	.expected_setns = EPERM,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(setns_cross_process, allowed) {
	/* clang-format on */
	.is_sandboxed = false,
	.expected_setns = 0,
};

FIXTURE_SETUP(setns_cross_process)
{
}

FIXTURE_TEARDOWN(setns_cross_process)
{
}

/*
 * setns into a child's UTS namespace: when sandboxed with
 * LANDLOCK_PERM_NAMESPACE_ENTER denying UTS, the rule-based check
 * applies regardless of which process created the namespace.
 */
TEST_F(setns_cross_process, setns)
{
	int ruleset_fd, ns_fd, status;
	pid_t child;
	int pipe_parent[2], pipe_child[2];
	char buf, path[64];

	disable_caps(_metadata);

	/*
	 * Enable dumpable so the parent can read /proc/<child>/ns/uts.
	 * Without this, ptrace access checks (PTRACE_MODE_READ) prevent
	 * opening another process's namespace entries.
	 */
	ASSERT_EQ(0, prctl(PR_SET_DUMPABLE, 1, 0, 0, 0));

	ASSERT_EQ(0, pipe2(pipe_parent, O_CLOEXEC));
	ASSERT_EQ(0, pipe2(pipe_child, O_CLOEXEC));

	child = fork();
	ASSERT_LE(0, child);

	if (child == 0) {
		EXPECT_EQ(0, close(pipe_parent[1]));
		EXPECT_EQ(0, close(pipe_child[0]));

		/* Child: create a UTS namespace. */
		set_cap(_metadata, CAP_SYS_ADMIN);
		ASSERT_EQ(0, unshare(CLONE_NEWUTS));

		drop_caps(_metadata);
		ASSERT_EQ(0, prctl(PR_SET_DUMPABLE, 1, 0, 0, 0));

		/* Signal parent that the namespace is ready. */
		ASSERT_EQ(1, write(pipe_child[1], ".", 1));

		/* Wait for parent to finish testing. */
		ASSERT_EQ(1, read(pipe_parent[0], &buf, 1));
		_exit(_metadata->exit_code);
	}

	EXPECT_EQ(0, close(pipe_parent[0]));
	EXPECT_EQ(0, close(pipe_child[1]));

	/* Wait for child namespace. */
	ASSERT_EQ(1, read(pipe_child[0], &buf, 1));
	EXPECT_EQ(0, close(pipe_child[0]));

	/* Open the child's NS FD BEFORE creating the domain. */
	snprintf(path, sizeof(path), "/proc/%d/ns/uts", child);
	ns_fd = open(path, O_RDONLY);
	ASSERT_LE(0, ns_fd);

	if (variant->is_sandboxed) {
		/* Create domain denying UTS entry (no allow rule). */
		ruleset_fd = create_ns_ruleset();
		ASSERT_LE(0, ruleset_fd);
		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	set_cap(_metadata, CAP_SYS_ADMIN);
	if (variant->expected_setns) {
		EXPECT_EQ(-1, setns(ns_fd, CLONE_NEWUTS));
		EXPECT_EQ(variant->expected_setns, errno);
	} else {
		EXPECT_EQ(0, setns(ns_fd, CLONE_NEWUTS));
	}
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, close(ns_fd));

	/* Release child. */
	ASSERT_EQ(1, write(pipe_parent[1], ".", 1));
	EXPECT_EQ(0, close(pipe_parent[1]));
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
}

/*
 * Verify that both LANDLOCK_PERM_NAMESPACE_ENTER and LANDLOCK_PERM_CAPABILITY_USE
 * apply simultaneously: creating/entering a non-user namespace
 * requires both the namespace type to be allowed AND CAP_SYS_ADMIN
 * to be allowed.  User namespace creation is the exception (no
 * capable() call from the kernel).
 */
TEST(setns_and_create)
{
	int ruleset_fd, ns_fd;
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_NAMESPACE_ENTER |
				LANDLOCK_PERM_CAPABILITY_USE,
	};
	const struct landlock_namespace_attr ns_attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
		.namespace_types = CLONE_NEWUTS,
	};
	const struct landlock_capability_attr cap_attr = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << CAP_SYS_ADMIN),
	};

	disable_caps(_metadata);

	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &ns_attr, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &cap_attr, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* UTS unshare: allowed by NS rule + CAP_SYS_ADMIN allowed. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, unshare(CLONE_NEWUTS));

	/* IPC unshare: denied by NS rule (type not allowed). */
	EXPECT_EQ(-1, unshare(CLONE_NEWIPC));
	EXPECT_EQ(EPERM, errno);

	/* setns into current UTS: allowed by NS rule. */
	ns_fd = open("/proc/self/ns/uts", O_RDONLY);
	ASSERT_LE(0, ns_fd);
	EXPECT_EQ(0, setns(ns_fd, CLONE_NEWUTS));
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, close(ns_fd));

	/*
	 * User namespace creation: only LANDLOCK_PERM_NAMESPACE_ENTER needed
	 * (no capable() call from the kernel for user NS).  Denied
	 * because CLONE_NEWUSER is not in the allowed namespace types.
	 */
	EXPECT_EQ(-1, unshare(CLONE_NEWUSER));
	EXPECT_EQ(EPERM, errno);
}

/*
 * Verify that LANDLOCK_PERM_CAPABILITY_USE can deny the CAP_SYS_ADMIN check
 * that the kernel performs before the Landlock namespace hook is
 * reached.  The NS type is allowed but the required capability is not,
 * so the operation fails on the capability check.
 *
 * User namespace creation is the exception: no capable() call, so the
 * operation succeeds with just LANDLOCK_PERM_NAMESPACE_ENTER.
 */
TEST(two_perm_cap_denied)
{
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_NAMESPACE_ENTER |
				LANDLOCK_PERM_CAPABILITY_USE,
	};
	const struct landlock_namespace_attr ns_attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
		.namespace_types = CLONE_NEWUTS | CLONE_NEWUSER,
	};
	/* CAP_SYS_ADMIN is NOT allowed. */
	const struct landlock_capability_attr cap_attr = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << CAP_SYS_CHROOT),
	};
	int ruleset_fd;

	disable_caps(_metadata);

	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &ns_attr, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &cap_attr, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/*
	 * UTS creation: the process holds CAP_SYS_ADMIN but Landlock
	 * denies it (not in the cap rule), so the kernel's
	 * ns_capable(CAP_SYS_ADMIN) gate fails before the namespace
	 * hook is reached.
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, unshare(CLONE_NEWUTS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/*
	 * User NS creation: no capable() call from the kernel, so
	 * only LANDLOCK_PERM_NAMESPACE_ENTER applies.  CLONE_NEWUSER is in the
	 * allowed set, so this succeeds.
	 */
	EXPECT_EQ(0, unshare(CLONE_NEWUSER));
}

/*
 * Mount namespace setns is unique: the kernel checks both
 * CAP_SYS_ADMIN and CAP_SYS_CHROOT in mntns_install().  Verify that
 * allowing CAP_SYS_ADMIN alone is not sufficient.
 */
TEST(two_perm_mnt_setns)
{
	const struct landlock_ruleset_attr attr = {
		.handled_perm = LANDLOCK_PERM_NAMESPACE_ENTER |
				LANDLOCK_PERM_CAPABILITY_USE,
	};
	const struct landlock_namespace_attr ns_attr = {
		.allowed_perm = LANDLOCK_PERM_NAMESPACE_ENTER,
		.namespace_types = CLONE_NEWNS,
	};
	const struct landlock_capability_attr cap_admin = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << CAP_SYS_ADMIN),
	};
	const struct landlock_capability_attr cap_admin_chroot = {
		.allowed_perm = LANDLOCK_PERM_CAPABILITY_USE,
		.capabilities = (1ULL << CAP_SYS_ADMIN) |
				(1ULL << CAP_SYS_CHROOT),
	};
	int ruleset_fd, ns_fd;

	disable_caps(_metadata);

	/* Layer 1: allow mount NS + CAP_SYS_ADMIN only (no CAP_SYS_CHROOT). */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &ns_attr, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &cap_admin, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	ns_fd = open("/proc/self/ns/mnt", O_RDONLY);
	ASSERT_LE(0, ns_fd);

	/*
	 * Fails: mntns_install() checks CAP_SYS_ADMIN (allowed) then
	 * CAP_SYS_CHROOT (denied by LANDLOCK_PERM_CAPABILITY_USE).
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	set_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(-1, setns(ns_fd, CLONE_NEWNS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	clear_cap(_metadata, CAP_SYS_CHROOT);

	/* Layer 2: also allows CAP_SYS_CHROOT. */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NAMESPACE,
				       &ns_attr, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_CAPABILITY,
				       &cap_admin_chroot, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/*
	 * Still fails: layer 1 still denies CAP_SYS_CHROOT.
	 * Landlock layer stacking means the most restrictive layer wins.
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	set_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(-1, setns(ns_fd, CLONE_NEWNS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	clear_cap(_metadata, CAP_SYS_CHROOT);
	EXPECT_EQ(0, close(ns_fd));
}

/* Audit tests */

static int matches_log_ns_create(int audit_fd, __u64 ns_type)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=perm\\.namespace_enter"
		" namespace_type=0x%x"
		" namespace_inum=0$";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len = snprintf(log_match, sizeof(log_match), log_template,
				 (unsigned int)ns_type);
	if (log_match_len >= sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_ACCESS, log_match,
				  NULL);
}

static int matches_log_ns_setns(int audit_fd, __u64 ns_type)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=perm\\.namespace_enter"
		" namespace_type=0x%x"
		" namespace_inum=[0-9]\\+$";
	char log_match[sizeof(log_template) + 10];
	int log_match_len;

	log_match_len = snprintf(log_match, sizeof(log_match), log_template,
				 (unsigned int)ns_type);
	if (log_match_len >= sizeof(log_match))
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_ACCESS, log_match,
				  NULL);
}

FIXTURE(ns_audit)
{
	struct audit_filter audit_filter;
	int audit_fd;
};

FIXTURE_SETUP(ns_audit)
{
	ASSERT_TRUE(is_in_init_user_ns());

	disable_caps(_metadata);

	set_cap(_metadata, CAP_AUDIT_CONTROL);
	self->audit_fd = audit_init_with_exe_filter(&self->audit_filter);
	EXPECT_LE(0, self->audit_fd);
	clear_cap(_metadata, CAP_AUDIT_CONTROL);
}

FIXTURE_TEARDOWN(ns_audit)
{
	set_cap(_metadata, CAP_AUDIT_CONTROL);
	EXPECT_EQ(0, audit_cleanup(self->audit_fd, &self->audit_filter));
}

/*
 * Verifies that a denied namespace creation produces the expected audit
 * record with the perm.namespace_enter blocker string and namespace_type.
 */
TEST_F(ns_audit, create_denied)
{
	struct audit_records records;
	int ruleset_fd;

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, unshare(CLONE_NEWUTS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);

	EXPECT_EQ(0, matches_log_ns_create(self->audit_fd, CLONE_NEWUTS));

	/*
	 * No extra access records: the denial was already consumed by
	 * matches_log_ns_create above.  One domain allocation record,
	 * emitted in the same event as the first access denial for this
	 * domain.
	 */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(ns_audit, create_allowed)
{
	struct audit_records records;
	int ruleset_fd;

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, add_ns_rule(ruleset_fd, CLONE_NEWUTS));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, unshare(CLONE_NEWUTS));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* No records: allowed operations never trigger audit logging. */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
}

TEST_F(ns_audit, setns_allowed)
{
	struct audit_records records;
	int ruleset_fd, ns_fd;

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, add_ns_rule(ruleset_fd, CLONE_NEWUTS));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	ns_fd = open("/proc/self/ns/uts", O_RDONLY);
	ASSERT_LE(0, ns_fd);

	/* Allowed: should succeed with no audit record. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, setns(ns_fd, CLONE_NEWUTS));
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, close(ns_fd));

	/* No records: allowed setns never triggers audit logging. */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
}

TEST_F(ns_audit, setns_denied)
{
	struct audit_records records;
	int ruleset_fd, ns_fd;

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	/* No rule allows UTS -> denied. */
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	ns_fd = open("/proc/self/ns/uts", O_RDONLY);
	ASSERT_LE(0, ns_fd);

	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, setns(ns_fd, CLONE_NEWUTS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, close(ns_fd));

	/* Verify the audit record for setns denial. */
	EXPECT_EQ(0, matches_log_ns_setns(self->audit_fd, CLONE_NEWUTS));

	/*
	 * No extra access records: the denial was already consumed by
	 * matches_log_ns_setns above.  One domain allocation record,
	 * emitted in the same event as the first access denial for this
	 * domain.
	 */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(ns_audit, unshare_denied)
{
	struct audit_records records;
	int ruleset_fd;

	ruleset_fd = create_ns_ruleset();
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* Deny UTS namespace creation (no allow rule). */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, unshare(CLONE_NEWUTS));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Verify the audit record for namespace creation denial. */
	EXPECT_EQ(0, matches_log_ns_create(self->audit_fd, CLONE_NEWUTS));

	/*
	 * No extra access records: the denial was already consumed by
	 * matches_log_ns_create above.  One domain allocation record,
	 * emitted in the same event as the first access denial for this
	 * domain.
	 */
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_HARNESS_MAIN
