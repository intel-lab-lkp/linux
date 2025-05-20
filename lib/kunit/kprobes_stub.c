// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit function redirection (kprobes stubbing) API.
 */

#include <kunit/test.h>
#include <kunit/kprobes_stub.h>

#include <linux/kprobes.h>

struct kunit_kprobes_stub_ctx {
	unsigned long real_fn_addr;
	unsigned long replacement_addr;
	struct kprobe kp;
};

static void __kunit_kprobes_stub_resource_free(struct kunit_resource *res)
{
	struct kunit_kprobes_stub_ctx *ctx = res->data;

	unregister_kprobe(&ctx->kp);
	kfree(ctx);
}

static int kprobe_handler(struct kprobe *kp, struct pt_regs *regs)
{
	struct kunit_kprobes_stub_ctx *ctx = container_of(kp, struct kunit_kprobes_stub_ctx, kp);

	instruction_pointer_set(regs, ctx->replacement_addr);
	return 1;
}

/* Matching function for kunit_find_resource().  match_data is real_fn_addr. */
static bool __kunit_kprobes_stub_resource_match(struct kunit *test,
						struct kunit_resource *res,
						void *match_real_fn_addr)
{
	struct kunit_kprobes_stub_ctx *ctx = res->data;

	/* Make sure the resource is a kprobes stub resource. */
	if (res->free != &__kunit_kprobes_stub_resource_free)
		return false;

	return ctx->real_fn_addr == (unsigned long)match_real_fn_addr;
}

void __kunit_activate_kprobes_stub(struct kunit *test,
				   const char *name,
				   void *real_fn_addr,
				   void *replacement_addr)
{
	struct kunit_kprobes_stub_ctx *ctx;
	struct kunit_resource *res;

	KUNIT_ASSERT_PTR_NE_MSG(test, real_fn_addr, NULL,
				"Tried to activate a stub for function NULL");

	/* If the replacement address is NULL, deactivate the stub. */
	if (!replacement_addr) {
		kunit_deactivate_kprobes_stub(test, real_fn_addr);
		return;
	}

	/* Look up any existing stubs for this function, and replace them. */
	res = kunit_find_resource(test,
				  __kunit_kprobes_stub_resource_match,
				  real_fn_addr);
	if (res) {
		ctx = res->data;
		ctx->replacement_addr = (unsigned long)replacement_addr;

		/* We got an extra reference from find_resource(), so put it. */
		kunit_put_resource(res);
	} else {
		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
		KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ctx, "Failed to allocate kunit stub for %s",
						 name);

		ctx->real_fn_addr = (unsigned long)real_fn_addr;
		ctx->replacement_addr = (unsigned long)replacement_addr;

		ctx->kp.addr = real_fn_addr;
		ctx->kp.pre_handler = kprobe_handler;
		KUNIT_ASSERT_EQ_MSG(test, register_kprobe(&ctx->kp), 0,
				    "Failed to allocate kunit stub for %s", name);

		kunit_alloc_resource(test, NULL,
				     __kunit_kprobes_stub_resource_free,
				     GFP_KERNEL, ctx);
	}
}
EXPORT_SYMBOL_GPL(__kunit_activate_kprobes_stub);

void kunit_deactivate_kprobes_stub(struct kunit *test, void *real_fn_addr)
{
	struct kunit_resource *res;

	KUNIT_ASSERT_PTR_NE_MSG(test, real_fn_addr, NULL, "Tried to deactivate a NULL stub.");

	res = kunit_find_resource(test,
				  __kunit_kprobes_stub_resource_match,
				  real_fn_addr);
	KUNIT_ASSERT_PTR_NE_MSG(test, res, NULL,
				"Tried to deactivate a nonexistent stub.");

	/*
	 * Free the stub. We 'put' twice, as we got a reference
	 * from kunit_find_resource()
	 */
	kunit_remove_resource(test, res);
	kunit_put_resource(res);
}
EXPORT_SYMBOL_GPL(kunit_deactivate_kprobes_stub);
