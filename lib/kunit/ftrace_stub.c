// SPDX-License-Identifier: GPL-2.0
#include <kunit/ftrace_stub.h>
#include <kunit/test.h>

#include <linux/typecheck.h>

#include <linux/ftrace.h>
#include <linux/livepatch.h>
#include <linux/sched.h>


struct kunit_ftrace_stub_ctx {
	struct kunit *test;
	unsigned long real_fn_addr; /* used as a key to lookup the stub */
	unsigned long replacement_addr;
	struct ftrace_ops ops; /* a copy of kunit_stub_base_ops with .private set */
};

static void kunit_stub_trampoline(unsigned long ip, unsigned long parent_ip,
				  struct ftrace_ops *ops,
				  struct ftrace_regs *fregs)
{
	struct kunit_ftrace_stub_ctx *ctx = ops->private;
	int lock_bit;

	if (current->kunit_test != ctx->test)
		return;

	lock_bit = ftrace_test_recursion_trylock(ip, parent_ip);
	KUNIT_ASSERT_GE(ctx->test, lock_bit, 0);

	ftrace_regs_set_instruction_pointer(fregs, ctx->replacement_addr);

	ftrace_test_recursion_unlock(lock_bit);
}

static struct ftrace_ops kunit_stub_base_ops = {
	.func = &kunit_stub_trampoline,
	.flags = FTRACE_OPS_FL_IPMODIFY |
#ifndef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS
		FTRACE_OPS_FL_SAVE_REGS |
#endif
		FTRACE_OPS_FL_DYNAMIC
};

static void __kunit_ftrace_stub_resource_free(struct kunit_resource *res)
{
	struct kunit_ftrace_stub_ctx *ctx = res->data;

	unregister_ftrace_function(&ctx->ops);
	kfree(ctx);
}

/* Matching function for kunit_find_resource(). match_data is real_fn_addr. */
static bool __kunit_static_stub_resource_match(struct kunit *test,
						struct kunit_resource *res,
						void *match_real_fn_addr)
{
	/* This pointer is only valid if res is a static stub resource. */
	struct kunit_ftrace_stub_ctx *ctx = res->data;

	/* Make sure the resource is a static stub resource. */
	if (res->free != &__kunit_ftrace_stub_resource_free)
		return false;

	return ctx->real_fn_addr == (unsigned long)match_real_fn_addr;
}

void kunit_deactivate_ftrace_stub(struct kunit *test, void *real_fn_addr)
{
	struct kunit_resource *res;

	KUNIT_ASSERT_PTR_NE_MSG(test, real_fn_addr, NULL,
				"Tried to deactivate a NULL stub.");

	/* Look up the existing stub for this function. */
	res = kunit_find_resource(test,
				  __kunit_static_stub_resource_match,
				  real_fn_addr);

	/* Error out if the stub doesn't exist. */
	KUNIT_ASSERT_PTR_NE_MSG(test, res, NULL,
				"Tried to deactivate a nonexistent stub.");

	/* Free the stub. We 'put' twice, as we got a reference
	 * from kunit_find_resource(). The free function will deactivate the
	 * ftrace stub.
	 */
	kunit_remove_resource(test, res);
	kunit_put_resource(res);
}
EXPORT_SYMBOL_GPL(kunit_deactivate_ftrace_stub);

void __kunit_activate_ftrace_stub(struct kunit *test,
				  const char *name,
				  void *real_fn_addr,
				  void *replacement_addr)
{
	unsigned long ftrace_ip;
	struct kunit_ftrace_stub_ctx *ctx;
	int ret;

	ftrace_ip = ftrace_location((unsigned long)real_fn_addr);
	if (!ftrace_ip)
		KUNIT_FAIL_ASSERTION(test, KUNIT_ASSERTION,
			"%s ip is invalid: not a function, or is marked notrace or inline", name);

	/* Allocate the stub context, which contains pointers to the replacement
	 * function and the test object. It's also registered as a KUnit
	 * resource which can be looked up by address (to deactivate manually)
	 * and is destroyed automatically on test exit.
	 */
	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_PTR_NE_MSG(test, ctx, NULL, "failed to allocate kunit stub for %s", name);

	ctx->test = test;
	ctx->ops = kunit_stub_base_ops;
	ctx->ops.private = ctx;
	ctx->real_fn_addr = (unsigned long)real_fn_addr;
	ctx->replacement_addr = (unsigned long)replacement_addr;

	ret = ftrace_set_filter_ip(&ctx->ops, ftrace_ip, 0, 0);
	if (ret) {
		kfree(ctx);
		KUNIT_FAIL_ASSERTION(test, KUNIT_ASSERTION,
				     "failed to set filter ip for %s: %d", name, ret);
	}

	ret = register_ftrace_function(&ctx->ops);
	if (ret) {
		kfree(ctx);
		if (ret == -EBUSY)
			KUNIT_FAIL_ASSERTION(
				test, KUNIT_ASSERTION,
				"failed to register stub (-EBUSY) for %s, likely due to already stubbing it?",
				name);
		KUNIT_FAIL_ASSERTION(test, KUNIT_ASSERTION,
				     "failed to register stub for %s: %d", name,
				     ret);
	}

	kunit_alloc_resource(test, NULL,
			     __kunit_ftrace_stub_resource_free,
			     GFP_KERNEL, ctx);
}
EXPORT_SYMBOL_GPL(__kunit_activate_ftrace_stub);
