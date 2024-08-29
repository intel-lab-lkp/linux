/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit function redirection (static stubbing) API.
 *
 * Copyright (C) 2022, Google LLC.
 * Author: David Gow <davidgow@google.com>
 */
#ifndef _KUNIT_STATIC_STUB_H
#define _KUNIT_STATIC_STUB_H

#if !IS_ENABLED(CONFIG_KUNIT)

/* If CONFIG_KUNIT is not enabled, these stubs quietly disappear. */
#define KUNIT_STATIC_STUB_REDIRECT(real_fn_name, args...) do {} while (0)
#define KUNIT_GLOBAL_STUB_REDIRECT(stub_name, args...) do {} while (0)
#define KUNIT_DECLARE_GLOBAL_STUB(stub_name, stub_type)

#else

#include <kunit/test.h>
#include <kunit/test-bug.h>

#include <linux/cleanup.h> /* for CLASS */
#include <linux/compiler.h> /* for {un,}likely() */
#include <linux/sched.h> /* for task_struct */


/**
 * KUNIT_STATIC_STUB_REDIRECT() - call a replacement 'static stub' if one exists
 * @real_fn_name: The name of this function (as an identifier, not a string)
 * @args: All of the arguments passed to this function
 *
 * This is a function prologue which is used to allow calls to the current
 * function to be redirected by a KUnit test. KUnit tests can call
 * kunit_activate_static_stub() to pass a replacement function in. The
 * replacement function will be called by KUNIT_STATIC_STUB_REDIRECT(), which
 * will then return from the function. If the caller is not in a KUnit context,
 * the function will continue execution as normal.
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	int real_func(int n)
 *	{
 *		KUNIT_STATIC_STUB_REDIRECT(real_func, n);
 *		return 0;
 *	}
 *
 *	int replacement_func(int n)
 *	{
 *		return 42;
 *	}
 *
 *	void example_test(struct kunit *test)
 *	{
 *		kunit_activate_static_stub(test, real_func, replacement_func);
 *		KUNIT_EXPECT_EQ(test, real_func(1), 42);
 *	}
 *
 */
#define KUNIT_STATIC_STUB_REDIRECT(real_fn_name, args...)		\
do {									\
	typeof(&real_fn_name) replacement;				\
	struct kunit *current_test = kunit_get_current_test();		\
									\
	if (likely(!current_test))					\
		break;							\
									\
	replacement = kunit_hooks.get_static_stub_address(current_test,	\
							&real_fn_name);	\
									\
	if (unlikely(replacement))					\
		return replacement(args);				\
} while (0)

/* Helper function for kunit_activate_static_stub(). The macro does
 * typechecking, so use it instead.
 */
void __kunit_activate_static_stub(struct kunit *test,
				  void *real_fn_addr,
				  void *replacement_addr);

/**
 * kunit_activate_static_stub() - replace a function using static stubs.
 * @test: A pointer to the 'struct kunit' test context for the current test.
 * @real_fn_addr: The address of the function to replace.
 * @replacement_addr: The address of the function to replace it with.
 *
 * When activated, calls to real_fn_addr from within this test (even if called
 * indirectly) will instead call replacement_addr. The function pointed to by
 * real_fn_addr must begin with the static stub prologue in
 * KUNIT_STATIC_STUB_REDIRECT() for this to work. real_fn_addr and
 * replacement_addr must have the same type.
 *
 * The redirection can be disabled again with kunit_deactivate_static_stub().
 */
#define kunit_activate_static_stub(test, real_fn_addr, replacement_addr) do {	\
	typecheck_fn(typeof(&replacement_addr), real_fn_addr);			\
	__kunit_activate_static_stub(test, real_fn_addr, replacement_addr);	\
} while (0)


/**
 * kunit_deactivate_static_stub() - disable a function redirection
 * @test: A pointer to the 'struct kunit' test context for the current test.
 * @real_fn_addr: The address of the function to no-longer redirect
 *
 * Deactivates a redirection configured with kunit_activate_static_stub(). After
 * this function returns, calls to real_fn_addr() will execute the original
 * real_fn, not any previously-configured replacement.
 */
void kunit_deactivate_static_stub(struct kunit *test, void *real_fn_addr);

/**
 * struct kunit_global_stub - Represents a context of global function stub.
 * @replacement: The address of replacement function.
 * @owner: The KUnit test that owns the stub, valid only when @busy > 0.
 * @busy: The stub busyness counter incremented on entry to the replacement
 *        function, decremented on exit, used to signal if the stub is idle.
 * @idle: The completion state to indicate when the stub is idle again.
 *
 * This structure is for KUnit internal use only.
 * See KUNIT_DECLARE_GLOBAL_STUB().
 */
struct kunit_global_stub {
	void *replacement;
	struct kunit *owner;
	atomic_t busy;
	struct completion idle;
};

/**
 * KUNIT_DECLARE_GLOBAL_STUB() - Declare a global function stub.
 * @stub_name: The name of the stub, must be a valid identifier
 * @stub_type: The type of the function that this stub will replace
 *
 * This macro will declare new identifier of an anonymous type that will
 * represent global stub function that could be used by KUnit. It can be stored
 * outside of the KUnit code. If the CONFIG_KUNIT is not enabled this will
 * be evaluated to an empty statement.
 *
 * The anonymous type introduced by this macro is mostly a wrapper to generic
 * struct kunit_global_stub but with additional dummy member, that is never
 * used directly, but is needed to maintain the type of the stub function.
 */
#define KUNIT_DECLARE_GLOBAL_STUB(stub_name, stub_type)				\
union {										\
	struct kunit_global_stub base;						\
	typeof(stub_type) dummy;						\
} stub_name

/* Internal struct to define guard class */
struct kunit_global_stub_guard {
	struct kunit_global_stub *stub;
	void *active_replacement;
};

/* Internal class used to guard stub calls */
DEFINE_CLASS(kunit_global_stub_guard,
	     struct kunit_global_stub_guard,
	     ({
		struct kunit_global_stub *stub = _T.stub;
		bool active = !!_T.active_replacement;

		if (active && !atomic_dec_return(&stub->busy))
			complete_all(&stub->idle);
	     }),
	     ({
		class_kunit_global_stub_guard_t guard;
		bool active = !!atomic_inc_not_zero(&stub->busy);

		guard.stub = stub;
		guard.active_replacement = active ? READ_ONCE(stub->replacement) : NULL;

		guard;
	     }),
	     struct kunit_global_stub *stub)

/**
 * KUNIT_GLOBAL_STUB_REDIRECT() - Call a global function stub if activated.
 * @stub: The function stub declared using KUNIT_DECLARE_GLOBAL_STUB()
 * @args: All of the arguments passed to this stub
 *
 * This is a function prologue which is used to allow calls to the current
 * function to be redirected if a KUnit is running. If the KUnit is not
 * running or stub is not yet activated the function will continue execution
 * as normal.
 *
 * The function stub must be declared with KUNIT_DECLARE_GLOBAL_STUB() that is
 * stored in a place that is accessible from both the test code, which will
 * activate this stub using kunit_activate_global_stub(), and from the function,
 * where we will do this redirection using KUNIT_GLOBAL_STUB_REDIRECT().
 *
 * Unlike the KUNIT_STATIC_STUB_REDIRECT(), this redirection will work
 * even if the caller is not in a KUnit context (like a worker thread).
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	KUNIT_DECLARE_GLOBAL_STUB(func_stub, int (*)(int n));
 *
 *	int real_func(int n)
 *	{
 *		KUNIT_GLOBAL_STUB_REDIRECT(func_stub, n);
 *		return n + 1;
 *	}
 *
 *	int replacement_func(int n)
 *	{
 *		return n + 100;
 *	}
 *
 *	void example_test(struct kunit *test)
 *	{
 *		KUNIT_EXPECT_EQ(test, real_func(1), 2);
 *		kunit_activate_global_stub(test, func_stub, replacement_func);
 *		KUNIT_EXPECT_EQ(test, real_func(1), 101);
 *	}
 */
#define KUNIT_GLOBAL_STUB_REDIRECT(stub, args...) do {					\
	if (kunit_is_running()) {							\
		typeof(stub) *__stub = &(stub);						\
		CLASS(kunit_global_stub_guard, guard)(&__stub->base);			\
		typeof(__stub->dummy) replacement = guard.active_replacement;		\
		if (unlikely(replacement)) {						\
			kunit_info(__stub->base.owner, "%s: redirecting to %ps\n",	\
				   __func__, replacement);				\
			return replacement(args);					\
		}									\
	}										\
} while (0)

void __kunit_activate_global_stub(struct kunit *test, struct kunit_global_stub *stub,
				  void *replacement_addr);

/**
 * kunit_activate_global_stub() - Setup a global function stub.
 * @test: Test case that wants to activate a global function stub
 * @stub: The location of the function stub pointer
 * @replacement: The replacement function
 *
 * This helper setups a function stub with the replacement function.
 * It will also automatically deactivate the stub at the test end.
 *
 * The redirection can be disabled with kunit_deactivate_global_stub().
 * The stub must be declared using KUNIT_DECLARE_GLOBAL_STUB().
 */
#define kunit_activate_global_stub(test, stub, replacement) do {		\
	typeof(stub) *__stub = &(stub);						\
	typecheck_fn(typeof(__stub->dummy), (replacement));			\
	__kunit_activate_global_stub((test), &__stub->base, (replacement));	\
} while (0)

void __kunit_deactivate_global_stub(struct kunit *test, struct kunit_global_stub *stub);

/**
 * kunit_deactivate_global_stub() - Disable a global function stub.
 * @test: Test case that wants to deactivate a global function stub
 * @stub: The location of the function stub pointer
 *
 * The stub must be declared using KUNIT_DECLARE_GLOBAL_STUB().
 */
#define kunit_deactivate_global_stub(test, stub) do {				\
	typeof(stub) *__stub = &(stub);						\
	__kunit_deactivate_global_stub((test), &__stub->base);			\
} while (0)

#endif
#endif
