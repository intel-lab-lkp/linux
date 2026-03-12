// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock - Namespace hooks
 *
 * Copyright © 2026 Cloudflare
 */

#include <linux/lsm_audit.h>
#include <linux/lsm_hooks.h>
#include <linux/ns/ns_common_types.h>
#include <linux/ns_common.h>
#include <linux/nsproxy.h>
#include <uapi/linux/landlock.h>

#include "audit.h"
#include "cred.h"
#include "limits.h"
#include "ns.h"
#include "ruleset.h"
#include "setup.h"

/* Ensures the audit inum field can hold ns_common.inum without truncation. */
static_assert(sizeof(((struct common_audit_data *)NULL)->u.ns.inum) >=
	      sizeof(((struct ns_common *)NULL)->inum));

static const struct access_masks ns_perm = {
	.perm = LANDLOCK_PERM_NAMESPACE_ENTER,
};

/**
 * hook_namespace_alloc - Check namespace entry permission for creation
 *
 * @ns: The namespace being initialized.
 *
 * Checks if the current domain allows entering (creating) this namespace
 * type.  Fires during unshare(2) and clone(2) via __ns_common_init() in
 * kernel/nscommon.c.
 *
 * Return: 0 if allowed, -EPERM if namespace creation is denied.
 */
static int hook_namespace_alloc(struct ns_common *const ns)
{
	const struct landlock_cred_security *subject;
	size_t denied_layer;

	WARN_ON_ONCE(!(CLONE_NS_ALL & ns->ns_type));

	subject =
		landlock_get_applicable_subject(current_cred(), ns_perm, NULL);
	if (!subject)
		return 0;

	denied_layer = landlock_perm_is_denied(
		subject->domain, LANDLOCK_PERM_NAMESPACE_ENTER,
		landlock_ns_type_to_bit(ns->ns_type));
	if (!denied_layer)
		return 0;

	landlock_log_denial(subject, &(struct landlock_request){
					     .type = LANDLOCK_REQUEST_NAMESPACE,
					     .audit.type = LSM_AUDIT_DATA_NS,
					     .audit.u.ns.ns_type = ns->ns_type,
					     .layer_plus_one = denied_layer,
				     });
	return -EPERM;
}

/**
 * hook_namespace_install - Check namespace entry permission
 *
 * @nsset: The namespace set being modified.
 * @ns: The namespace being entered.
 *
 * Checks if the current domain restricts entering this namespace type.
 * Fires during setns(2) via validate_ns() in kernel/nsproxy.c.
 * Uses the same type-based check as hook_namespace_alloc(): the
 * restriction is on which namespace types the process can enter,
 * regardless of who created the namespace.
 *
 * Return: 0 if entry is allowed, -EPERM if denied.
 */
static int hook_namespace_install(const struct nsset *nsset,
				  struct ns_common *ns)
{
	const struct landlock_cred_security *subject;
	size_t denied_layer;

	WARN_ON_ONCE(!(CLONE_NS_ALL & ns->ns_type));

	subject =
		landlock_get_applicable_subject(current_cred(), ns_perm, NULL);
	if (!subject)
		return 0;

	denied_layer = landlock_perm_is_denied(
		subject->domain, LANDLOCK_PERM_NAMESPACE_ENTER,
		landlock_ns_type_to_bit(ns->ns_type));
	if (!denied_layer)
		return 0;

	landlock_log_denial(subject, &(struct landlock_request){
					     .type = LANDLOCK_REQUEST_NAMESPACE,
					     .audit.type = LSM_AUDIT_DATA_NS,
					     .audit.u.ns.ns_type = ns->ns_type,
					     .audit.u.ns.inum = ns->inum,
					     .layer_plus_one = denied_layer,
				     });
	return -EPERM;
}

static struct security_hook_list landlock_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(namespace_alloc, hook_namespace_alloc),
	LSM_HOOK_INIT(namespace_install, hook_namespace_install),
};

__init void landlock_add_ns_hooks(void)
{
	security_add_hooks(landlock_hooks, ARRAY_SIZE(landlock_hooks),
			   &landlock_lsmid);
}

#ifdef CONFIG_SECURITY_LANDLOCK_KUNIT_TEST

#include <kunit/test.h>

/* clang-format off */
#define _TEST_NS_BIT(struct_name, flag) \
	do { \
		const u64 bit = landlock_ns_type_to_bit(flag); \
		KUNIT_EXPECT_NE(test, 0ULL, bit); \
		KUNIT_EXPECT_EQ(test, 0ULL, seen &bit); \
		seen |= bit; \
	} while (0);
/* clang-format on */

static void test_ns_type_to_bit(struct kunit *const test)
{
	u64 seen = 0;

	FOR_EACH_NS_TYPE(_TEST_NS_BIT)

	KUNIT_EXPECT_EQ(test, GENMASK_ULL(LANDLOCK_NUM_PERM_NS - 1, 0), seen);
}

static void test_ns_type_to_bit_unknown(struct kunit *const test)
{
	KUNIT_EXPECT_EQ(test, 0ULL, landlock_ns_type_to_bit(CLONE_THREAD));
}

static void test_ns_types_to_bits_all(struct kunit *const test)
{
	KUNIT_EXPECT_EQ(test, GENMASK_ULL(LANDLOCK_NUM_PERM_NS - 1, 0),
			landlock_ns_types_to_bits(CLONE_NS_ALL));
}

/* clang-format off */
#define _TEST_NS_SINGLE(struct_name, flag) \
	KUNIT_EXPECT_EQ(test, landlock_ns_type_to_bit(flag), \
			landlock_ns_types_to_bits(flag));
/* clang-format on */

static void test_ns_types_to_bits_single(struct kunit *const test)
{
	FOR_EACH_NS_TYPE(_TEST_NS_SINGLE)
}

static void test_ns_types_to_bits_zero(struct kunit *const test)
{
	KUNIT_EXPECT_EQ(test, 0ULL, landlock_ns_types_to_bits(0));
}

static struct kunit_case test_cases[] = {
	KUNIT_CASE(test_ns_type_to_bit),
	KUNIT_CASE(test_ns_type_to_bit_unknown),
	KUNIT_CASE(test_ns_types_to_bits_all),
	KUNIT_CASE(test_ns_types_to_bits_single),
	KUNIT_CASE(test_ns_types_to_bits_zero),
	{}
};

static struct kunit_suite test_suite = {
	.name = "landlock_ns",
	.test_cases = test_cases,
};

kunit_test_suite(test_suite);

#endif /* CONFIG_SECURITY_LANDLOCK_KUNIT_TEST */
