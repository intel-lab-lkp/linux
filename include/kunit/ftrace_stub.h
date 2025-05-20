/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KUNIT_FTRACE_STUB_H
#define _KUNIT_FTRACE_STUB_H

/** KUNIT_STUBBABLE - marks a function as stubbable when stubbing support is
 * enabled.
 *
 * Stubbing uses ftrace internally, so we can only stub out functions when they
 * are not inlined. This macro evaluates to noinline when stubbing support is
 * enabled to thus make it safe.
 *
 * If you cannot add this annotation to the function, you can instead use
 * KUNIT_STUBBABLE_TRAMPOLINE, which is the same, but evaluates to
 * __always_inline when stubbing is not enabled.
 *
 * Consider copy_to_user, which is marked as __always_inline:
 *
 * .. code-block:: c
 *	static KUNIT_STUBBABLE_TRAMPOLINE unsigned long
 *	copy_to_user_trampoline(void __user *to, const void *from, unsigned long n)
 *	{
 *		return copy_to_user(to, from, n);
 *	}
 *
 * Then we simply need to update our code to go through this function instead
 * (in the places where we want to stub it out).
 */
#if IS_ENABLED(CONFIG_KUNIT_FTRACE_STUBS)
#define KUNIT_STUBBABLE noinline
#define KUNIT_STUBBABLE_TRAMPOLINE noinline
#else
#define KUNIT_STUBBABLE
#define KUNIT_STUBBABLE_TRAMPOLINE __always_inline
#endif

struct kunit;

/**
 * kunit_activate_ftrace_stub() - makes all calls to @func go to @replacement during @test.
 * @test: The test context object.
 * @func: The function to stub out, must be annotated with KUNIT_STUBBABLE.
 * @replacement: The function to replace @func with.
 *
 * All calls to @func will instead call @replacement for the duration of the
 * current test. If called from outside the test's thread, the function will
 * not be redirected.
 *
 * The redirection can be disabled again with kunit_deactivate_ftrace_stub().
 *
 * Example:
 *
 * .. code-block:: c
 *	KUNIT_STUBBABLE int real_func(int n)
 *	{
 *		pr_info("real_func() called with %d", n);
 *		return 0;
 *	}
 *
 *	void replacement_func(int n)
 *	{
 *		pr_info("replacement_func() called with %d", n);
 *		return 42;
 *	}
 *
 *	void example_test(struct kunit *test)
 *	{
 *		kunit_activate_ftrace_stub(test, real_func, replacement_func);
 *		KUNIT_EXPECT_EQ(test, real_func(1), 42);
 *	}
 *
 */
#define kunit_activate_ftrace_stub(test, func, replacement) do { \
	typecheck_fn(typeof(&func), replacement); \
	__kunit_activate_ftrace_stub(test, #func, func, replacement); \
} while (0)

void __kunit_activate_ftrace_stub(struct kunit *test,
				  const char *name,
				  void *real_fn_addr,
				  void *replacement_addr);


void kunit_deactivate_ftrace_stub(struct kunit *test, void *real_fn_addr);
#endif  /* _KUNIT_STUB_H */
