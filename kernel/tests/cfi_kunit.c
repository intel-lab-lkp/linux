// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for Kernel Control Flow Integrity (kCFI).
 *
 * Exercises properties of the compiler's KCFI indirect-call checks:
 *
 * Mirrors drivers/misc/lkdtm/cfi.c's CFI_FORWARD_PROTO test, but as a
 * self-contained kunit suite that drives kernel/cfi.c via the standard
 * indirect-call path. For the fatal-trap behavior of a real violation, see
 * LKDTM's "CFI_*" tests.
 */

#include <kunit/test.h>
#include <kunit/test-bug.h>
#include <linux/cfi.h>

/*
 * The test case currently expecting to count kCFI traps, and its running
 * count. Only ever touched for the test that armed cfi_kunit_active, so a
 * single counter is safe without locking.
 */
static struct kunit *cfi_kunit_active;
static int cfi_kunit_trap_count;

/*
 * Consulted from report_cfi_failure() in kCFI trap context, which may be
 * NMI-like (e.g. riscv kernel breakpoints), so this must stay lock-free: it
 * only reads the current task's kunit pointer and touches module-static
 * counters. It claims the failure by counting it and asking the arch handler
 * to skip the trap and resume, but only when the trap fired on the very test
 * that armed us. Any other CFI failure is left to behave normally.
 */
static bool cfi_kunit_failure_hook(void)
{
	struct kunit *test = READ_ONCE(cfi_kunit_active);

	/*
	 * Claim the failure only when a test is armed and it is the one
	 * running on this thread. Without the NULL check, a real CFI violation
	 * on a background thread (where kunit_get_current_test() is also NULL)
	 * while no test is active would match and be wrongly suppressed.
	 */
	if (!test || kunit_get_current_test() != test)
		return false;

	WRITE_ONCE(cfi_kunit_trap_count, cfi_kunit_trap_count + 1);
	return true;
}

static int called_count;

/*
 * Two same-arity, same-arg-type callees with deliberately different return
 * types so that kCFI's type-hash check at the call site catches the cast.
 */
static noinline void cfi_increment_void(int *counter)
{
	(*counter)++;
}

static noinline int cfi_increment_int(int *counter)
{
	(*counter)++;
	return *counter;
}

/*
 * The indirect call site. Type of the function pointer is what kCFI
 * compares against the hash baked into the callee's __cfi_<name> prefix.
 */
static noinline void cfi_indirect_call(void (*func)(int *))
{
	func(&called_count);
}

/*
 * Increasing-arity callees. Each returns a position-weighted sum of its
 * arguments so that a dropped, reordered, or zeroed argument produces a wrong
 * result rather than a coincidental match. Called with args (1, 2, 3, ...),
 * cfi_arityN() returns sum(i*i) for i in 1..N.
 */
static noinline int cfi_arity1(int a)
{
	return a;
}

static noinline int cfi_arity2(int a, int b)
{
	return a + 2 * b;
}

static noinline int cfi_arity3(int a, int b, int c)
{
	return a + 2 * b + 3 * c;
}

static noinline int cfi_arity4(int a, int b, int c, int d)
{
	return a + 2 * b + 3 * c + 4 * d;
}

static noinline int cfi_arity5(int a, int b, int c, int d, int e)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e;
}

static noinline int cfi_arity6(int a, int b, int c, int d, int e, int f)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

static noinline int cfi_arity7(int a, int b, int c, int d, int e, int f, int g)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

/*
 * Tail-calling trampolines: each receives the callee as an opaque pointer
 * (defeating optimization) plus the arguments, then `return fn(args)` as
 * its final statement so the compiler lowers it to an indirect tail call.
 * Arity grows so the callee pointer and the kCFI scratch registers
 * increasingly contend with argument registers.
 */
static noinline int cfi_tail_call1(int (*fn)(int), int a)
{
	return fn(a);
}

static noinline int cfi_tail_call2(int (*fn)(int, int), int a, int b)
{
	return fn(a, b);
}

static noinline int cfi_tail_call3(int (*fn)(int, int, int),
				   int a, int b, int c)
{
	return fn(a, b, c);
}

static noinline int cfi_tail_call4(int (*fn)(int, int, int, int),
				   int a, int b, int c, int d)
{
	return fn(a, b, c, d);
}

static noinline int cfi_tail_call5(int (*fn)(int, int, int, int, int),
				   int a, int b, int c, int d, int e)
{
	return fn(a, b, c, d, e);
}

static noinline int cfi_tail_call6(int (*fn)(int, int, int, int, int, int),
				   int a, int b, int c, int d, int e, int f)
{
	return fn(a, b, c, d, e, f);
}

static noinline int cfi_tail_call7(int (*fn)(int, int, int, int, int, int, int),
				   int a, int b, int c, int d, int e, int f,
				   int g)
{
	return fn(a, b, c, d, e, f, g);
}

#define CFI_MAX_ARITY 7

static void cfi_kunit_forward_proto_traps(struct kunit *test)
{
	int before_traps = READ_ONCE(cfi_kunit_trap_count);

	/* Only this case needs kCFI; the well-typed cases below run regardless. */
	if (!IS_ENABLED(CONFIG_CFI))
		kunit_skip(test, "kCFI is not enabled (CONFIG_CFI=n)");

	/*
	 * Force a kCFI type mismatch: the call site expects a callee whose
	 * __cfi_ prefix encodes "void (*)(int *)", but the actual callee's
	 * prefix encodes "int (*)(int *)". The (void *) intermediate cast
	 * follows drivers/misc/lkdtm/cfi.c and sidesteps -Wcast-function-type
	 * on the deliberate mis-cast.
	 *
	 * kCFI must detect this. The failure hook counts the trap and lets us
	 * survive it, so control returns here normally.
	 */
	cfi_indirect_call((void *)cfi_increment_int);

	KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(cfi_kunit_trap_count), before_traps + 1,
			    "mismatched-prototype indirect call was not caught by kCFI\n");
}

static void cfi_kunit_baseline_matched_proto(struct kunit *test)
{
	int before_traps = READ_ONCE(cfi_kunit_trap_count);
	int before_calls = called_count;

	/* Matched prototype: must NOT trap and must increment the counter. */
	cfi_indirect_call(cfi_increment_void);
	KUNIT_EXPECT_EQ(test, called_count, before_calls + 1);
	KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(cfi_kunit_trap_count), before_traps,
			    "well-typed indirect call spuriously tripped kCFI\n");
}

static void cfi_kunit_arity_matched_calls(struct kunit *test)
{
	/* expected[N] = sum(i*i) for i in 1..N */
	static const int expected[CFI_MAX_ARITY + 1] = {
		0, 1, 5, 14, 30, 55, 91, 140,
	};
	int before_traps = READ_ONCE(cfi_kunit_trap_count);
	int results[CFI_MAX_ARITY + 1];
	int i;

	results[1] = cfi_tail_call1(cfi_arity1, 1);
	results[2] = cfi_tail_call2(cfi_arity2, 1, 2);
	results[3] = cfi_tail_call3(cfi_arity3, 1, 2, 3);
	results[4] = cfi_tail_call4(cfi_arity4, 1, 2, 3, 4);
	results[5] = cfi_tail_call5(cfi_arity5, 1, 2, 3, 4, 5);
	results[6] = cfi_tail_call6(cfi_arity6, 1, 2, 3, 4, 5, 6);
	results[7] = cfi_tail_call7(cfi_arity7, 1, 2, 3, 4, 5, 6, 7);

	for (i = 1; i <= CFI_MAX_ARITY; i++)
		KUNIT_EXPECT_EQ_MSG(test, results[i], expected[i],
				    "arity-%d matched indirect call returned %d, expected %d\n",
				    i, results[i], expected[i]);

	/*
	 * None of the matched calls may trip kCFI. A spurious trap here is a
	 * codegen bug, most likely the callee pointer never reaching the call
	 * register under argument-register pressure.
	 */
	KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(cfi_kunit_trap_count), before_traps,
			    "a matched-prototype indirect call tripped kCFI under register pressure (codegen bug)\n");
}

static int cfi_kunit_init(struct kunit *test)
{
	WRITE_ONCE(cfi_kunit_trap_count, 0);
	WRITE_ONCE(cfi_kunit_active, test);
	return 0;
}

static void cfi_kunit_exit(struct kunit *test)
{
	WRITE_ONCE(cfi_kunit_active, NULL);
}

static int cfi_kunit_suite_init(struct kunit_suite *suite)
{
	cfi_kunit_set_failure_hook(cfi_kunit_failure_hook);
	return 0;
}

static void cfi_kunit_suite_exit(struct kunit_suite *suite)
{
	cfi_kunit_set_failure_hook(NULL);
}

static struct kunit_case cfi_kunit_cases[] = {
	KUNIT_CASE(cfi_kunit_baseline_matched_proto),
	KUNIT_CASE(cfi_kunit_arity_matched_calls),
	KUNIT_CASE(cfi_kunit_forward_proto_traps),
	{}
};

static struct kunit_suite cfi_kunit_suite = {
	.name = "cfi",
	.init = cfi_kunit_init,
	.exit = cfi_kunit_exit,
	.suite_init = cfi_kunit_suite_init,
	.suite_exit = cfi_kunit_suite_exit,
	.test_cases = cfi_kunit_cases,
};
kunit_test_suite(cfi_kunit_suite);

MODULE_DESCRIPTION("KUnit tests for kCFI indirect-call type checks");
MODULE_LICENSE("GPL");
