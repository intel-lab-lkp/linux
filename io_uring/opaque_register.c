// SPDX-License-Identifier: GPL-2.0
/* Ring-local registration IDs for shared kernel-resident object stores. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/io_uring/opaque.h>

#include "io_uring.h"
#include "opaque.h"

static void io_opaque_unregister_user(struct io_opaque_store *store)
{
	io_opaque_user_put(store);
	io_opaque_put(store);
}

int io_register_opaque(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_opaque_register reg;
	struct io_uring_opaque_config config;
	struct io_opaque_store *store;
	u32 size, id;
	int ret;

	lockdep_assert_held(&ctx->uring_lock);
	if (!(ctx->flags & IORING_SETUP_DEFER_TASKRUN) ||
	    !(ctx->flags & (IORING_SETUP_CQE32 | IORING_SETUP_CQE_MIXED)))
		return -EINVAL;
	if (get_user(size, &((struct io_uring_opaque_register __user *)arg)->size))
		return -EFAULT;
	if (size < offsetof(struct io_uring_opaque_register, __resv) || size > PAGE_SIZE)
		return -EINVAL;
	ret = copy_struct_from_user(&reg, sizeof(reg), arg, size);
	if (ret)
		return ret;
	if (reg.size != size || reg.__resv0 ||
	    !mem_is_zero(reg.__resv, sizeof(reg.__resv)))
		return -EINVAL;
	reg.size = sizeof(reg);
	if (reg.store_id || reg.flags)
		return -EINVAL;
	if (reg.fd != -1 || !reg.config ||
	    reg.config_size < offsetof(struct io_uring_opaque_config, __resv0) ||
	    reg.config_size > PAGE_SIZE)
		return -EINVAL;
	ret = copy_struct_from_user(&config, sizeof(config),
				    u64_to_user_ptr(reg.config), reg.config_size);
	if (ret)
		return ret;
	store = io_opaque_alloc(ctx, &config);
	if (IS_ERR(store))
		return PTR_ERR(store);
	ret = xa_alloc(&ctx->opaque_stores, &id, store, xa_limit_31b, GFP_KERNEL);
	if (ret)
		goto put_store;
	reg.store_id = id;
	ret = copy_struct_to_user(arg, size, &reg, sizeof(reg), NULL);
	if (!ret)
		return 0;
	xa_erase(&ctx->opaque_stores, id);
put_store:
	io_opaque_unregister_user(store);
	return ret;
}

void io_terminate_opaque(struct io_ring_ctx *ctx)
{
	struct io_opaque_store *store;
	unsigned long index;

	lockdep_assert_held(&ctx->uring_lock);
	if (ctx->opaque_stores_dying)
		return;
	ctx->opaque_stores_dying = true;
	xa_for_each(&ctx->opaque_stores, index, store)
		io_opaque_user_put(store);
}

void io_unregister_opaque(struct io_ring_ctx *ctx)
{
	struct io_opaque_store *store;
	unsigned long index;

	lockdep_assert_held(&ctx->uring_lock);
	io_terminate_opaque(ctx);
	xa_for_each(&ctx->opaque_stores, index, store)
		io_opaque_put(store);
	xa_destroy(&ctx->opaque_stores);
}
