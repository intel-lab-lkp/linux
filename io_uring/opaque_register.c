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

static int io_opaque_box_release(struct inode *inode, struct file *file)
{
	io_opaque_unregister_user(file->private_data);
	return 0;
}

static const struct file_operations io_opaque_box_fops = {
	.owner = THIS_MODULE,
	.release = io_opaque_box_release,
};

static int io_opaque_export(struct io_ring_ctx *ctx, void __user *arg,
			    struct io_uring_opaque_register *reg, u32 size)
{
	struct io_opaque_store *store = xa_load(&ctx->opaque_stores, reg->store_id);
	struct file *file;
	int fd, ret;

	if (!store)
		return -ENXIO;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	io_opaque_get(store);
	file = anon_inode_create_getfile("[io_uring-opaque]", &io_opaque_box_fops,
					 store, O_CLOEXEC, NULL);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		io_opaque_unregister_user(store);
		goto put_fd;
	}
	reg->fd = fd;
	ret = copy_struct_to_user(arg, size, reg, sizeof(*reg), NULL);
	if (ret) {
		fput(file);
		goto put_fd;
	}
	fd_install(fd, file);
	return 0;
put_fd:
	put_unused_fd(fd);
	return ret;
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
	if (reg.flags == IORING_OPAQUE_REG_EXPORT) {
		if (reg.fd != -1 || reg.config || reg.config_size)
			return -EINVAL;
		return io_opaque_export(ctx, arg, &reg, size);
	}
	if (reg.store_id)
		return -EINVAL;
	if (reg.flags == IORING_OPAQUE_REG_IMPORT) {
		CLASS(fd, f)(reg.fd);

		if (reg.config || reg.config_size)
			return -EINVAL;
		if (fd_empty(f) || fd_file(f)->f_op != &io_opaque_box_fops)
			return -EBADF;
		store = fd_file(f)->private_data;
		io_opaque_get(store);
	} else if (!reg.flags) {
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
	} else {
		return -EINVAL;
	}
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
