// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for commoncap.c security functions
 *
 * Tests for security-critical functions in the capability subsystem,
 * particularly namespace-related capability checks.
 */

#include <kunit/test.h>
#include <linux/user_namespace.h>
#include <linux/uidgid.h>
#include <linux/module.h>

/* We need to include the actual vfsuid_t definition but avoid the problematic
 * inline functions in mnt_idmapping.h. Include just the type definitions.
 */
#include <linux/types.h>

/* Forward declare the minimal types we need - match the actual kernel definitions */
struct mnt_idmap;
typedef struct {
	uid_t val;
} vfsuid_t;

/* Minimal macros we need - match kernel definitions from mnt_idmapping.h */
static inline uid_t __vfsuid_val(vfsuid_t uid)
{
	return uid.val;
}

#define VFSUIDT_INIT(val) ((vfsuid_t){ __kuid_val(val) })
#define INVALID_VFSUID VFSUIDT_INIT(INVALID_UID)

#ifdef CONFIG_SECURITY_COMMONCAP_KUNIT_TEST

/* Forward declaration - function is exported when KUNIT_TEST is enabled */
extern bool rootid_owns_currentns(vfsuid_t rootvfsuid);

/**
 * test_rootid_owns_currentns_init_ns - Test rootid_owns_currentns with init ns
 *
 * Verifies that a root ID in the init namespace correctly owns the current
 * namespace when running in init_user_ns.
 */
static void test_rootid_owns_currentns_init_ns(struct kunit *test)
{
	vfsuid_t root_vfsuid;
	kuid_t root_kuid;

	/* Create a root UID in init namespace */
	root_kuid = KUIDT_INIT(0);
	root_vfsuid = VFSUIDT_INIT(root_kuid);

	/* In init namespace, root should own current namespace */
	KUNIT_EXPECT_TRUE(test, rootid_owns_currentns(root_vfsuid));
}

/**
 * test_rootid_owns_currentns_invalid - Test rootid_owns_currentns with invalid vfsuid
 *
 * Verifies that an invalid vfsuid correctly returns false.
 */
static void test_rootid_owns_currentns_invalid(struct kunit *test)
{
	vfsuid_t invalid_vfsuid;

	/* Use the predefined invalid vfsuid */
	invalid_vfsuid = INVALID_VFSUID;

	/* Invalid vfsuid should return false */
	KUNIT_EXPECT_FALSE(test, rootid_owns_currentns(invalid_vfsuid));
}

/**
 * test_rootid_owns_currentns_nonroot - Test rootid_owns_currentns with non-root UID
 *
 * Verifies that a non-root UID correctly returns false.
 */
static void test_rootid_owns_currentns_nonroot(struct kunit *test)
{
	vfsuid_t nonroot_vfsuid;
	kuid_t nonroot_kuid;

	/* Create a non-root UID */
	nonroot_kuid = KUIDT_INIT(1000);
	nonroot_vfsuid = VFSUIDT_INIT(nonroot_kuid);

	/* Non-root UID should return false */
	KUNIT_EXPECT_FALSE(test, rootid_owns_currentns(nonroot_vfsuid));
}

static struct kunit_case commoncap_test_cases[] = {
	KUNIT_CASE(test_rootid_owns_currentns_init_ns),
	KUNIT_CASE(test_rootid_owns_currentns_invalid),
	KUNIT_CASE(test_rootid_owns_currentns_nonroot),
	{}
};

static struct kunit_suite commoncap_test_suite = {
	.name = "commoncap",
	.test_cases = commoncap_test_cases,
};

kunit_test_suite(commoncap_test_suite);

MODULE_LICENSE("GPL");

#endif /* CONFIG_SECURITY_COMMONCAP_KUNIT_TEST */

