// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 KylinSoft Corporation.
 * Author: Chenghao Duan <duanchenghao@kylinos.cn>
 */

/**
 * DOC: Eventfd Preservation via LUO
 *
 * Overview
 * ========
 *
 * Event file descriptors (eventfd) can be preserved over a kexec using the Live
 * Update Orchestrator (LUO) file preservation. This allows userspace applications
 * that use eventfd for event notification to maintain their state across kernel
 * updates.
 *
 * Eventfd is a simple notification mechanism that uses a 64-bit counter for
 * signaling events between userspace processes or between userspace and kernel.
 * The preservation ensures that pending events and configuration are not lost
 * during kexec.
 *
 * The preservation is not intended to be transparent. Only select properties of
 * the eventfd are preserved. All others are reset to default. The preserved
 * properties are described below.
 *
 * Preserved Properties
 * ====================
 *
 * The following properties of the eventfd are preserved across kexec:
 *
 * Counter Value
 *   The current 64-bit counter value is preserved. This includes any pending
 *   events that have been signaled but not yet consumed by readers.
 *
 * File Flags
 *   The creation flags (EFD_SEMAPHORE, EFD_CLOEXEC, EFD_NONBLOCK) are preserved.
 *   These control the behavior of read/write operations and file descriptor
 *   inheritance.
 *
 * Non-Preserved Properties
 * ========================
 *
 * All properties which are not preserved must be assumed to be reset to
 * default. This section describes some of those properties which may be more of
 * note.
 *
 * File Descriptor Number
 *   The file descriptor number itself is not preserved. After restore, the
 *   eventfd will be assigned a new file descriptor number in the target process.
 *
 * Wait Queue State
 *   Any processes currently blocked on read() operations will be woken up and
 *   need to re-establish their blocking state if desired.
 *
 * File Position
 *   Eventfd files don't have a traditional file position, but any internal
 *   state related to the file descriptor is reset.
 */

#include <linux/err.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/eventfd.h>
#include <linux/liveupdate.h>
#include <linux/module.h>
#include <linux/eventfd.h>
#include <linux/anon_inodes.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kref.h>
#include <linux/fdtable.h>

static int eventfd_luo_preserve(struct liveupdate_file_op_args *args)
{
	struct eventfd_luo_ser *ser;
	u64 count;
	unsigned int flags;
	int err = 0;

	/* Get eventfd state safely */
	err = eventfd_luo_get_state(args->file, &count, &flags);
	if (err) {
		pr_err("Failed to get eventfd state: %d\n", err);
		return err;
	}

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		err = PTR_ERR(ser);
		pr_err("Failed to allocate preserve memory: %d\n", err);
		return err;
	}

	/* Save eventfd state */
	ser->count = count;
	ser->flags = flags;

	pr_debug("Preserved eventfd: count=%llu, flags=0x%x\n",
		ser->count, ser->flags);

	/* Return physical address of serialization structure */
	args->serialized_data = virt_to_phys(ser);

	return 0;
}

static int eventfd_luo_freeze(struct liveupdate_file_op_args *args)
{
	struct eventfd_luo_ser *ser;
	u64 count;
	unsigned int flags;
	int err;

	if (WARN_ON_ONCE(!args->serialized_data))
		return -EINVAL;

	ser = phys_to_virt(args->serialized_data);

	/* Get current state and update if changed */
	err = eventfd_luo_get_state(args->file, &count, &flags);
	if (err)
		return err;

	if (ser->count != count) {
		pr_debug("WARNING: Count changed during preserve->freeze! old=%llu, new=%llu\n",
			 ser->count, count);
	}

	ser->count = count;

	return 0;
}

static void eventfd_luo_unpreserve(struct liveupdate_file_op_args *args)
{
	struct eventfd_luo_ser *ser;

	if (WARN_ON_ONCE(!args->serialized_data))
		return;

	ser = phys_to_virt(args->serialized_data);
	kho_unpreserve_free(ser);
}

static int eventfd_luo_retrieve(struct liveupdate_file_op_args *args)
{
	struct eventfd_luo_ser *ser;
	struct eventfd_ctx *ctx;
	struct file *file = NULL;
	int eventfd;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return -EINVAL;

	/* Create a new eventfd with the preserved count and flags */
	eventfd = eventfd_create(ser->count, ser->flags);
	if (eventfd < 0) {
		pr_err("Failed to create eventfd: %d\n", eventfd);
		return eventfd;
	}

	file = fget(eventfd);
	if (!file) {
		pr_err("Failed to get file from fd\n");
		close_fd(eventfd);
		return -EBADF;
	}

	close_fd(eventfd);

	/* Verify the created file has correct internal state */
	ctx = eventfd_ctx_fileget(file);
	if (IS_ERR(ctx)) {
		pr_err("Failed to get context from file\n");
		fput(file);
		return PTR_ERR(ctx);
	}

	eventfd_ctx_put(ctx);

	args->file = file;
	return 0;
}

static void eventfd_luo_finish(struct liveupdate_file_op_args *args)
{
	struct eventfd_luo_ser *ser;

	if (args->retrieve_status)
		return;

	if (!args->serialized_data)
		return;

	ser = phys_to_virt(args->serialized_data);
	if (!ser)
		return;

	kho_restore_free(ser);
}

static bool eventfd_luo_can_preserve(struct liveupdate_file_handler *handler,
				     struct file *file)
{
	struct eventfd_ctx *ctx;

	if (!file->f_op)
		return false;

	/* Try to get eventfd context - this will fail if not an eventfd */
	ctx = eventfd_ctx_fileget(file);
	if (IS_ERR(ctx))
		return false;

	eventfd_ctx_put(ctx);
	return true;
}

static const struct liveupdate_file_ops eventfd_luo_file_ops = {
	.preserve = eventfd_luo_preserve,
	.unpreserve = eventfd_luo_unpreserve,
	.freeze = eventfd_luo_freeze,
	.retrieve = eventfd_luo_retrieve,
	.finish = eventfd_luo_finish,
	.can_preserve = eventfd_luo_can_preserve,
	.owner = THIS_MODULE,
};

static struct liveupdate_file_handler eventfd_luo_handler = {
	.ops = &eventfd_luo_file_ops,
	.compatible = EVENTFD_LUO_FH_COMPATIBLE,
};

static int __init eventfd_luo_init(void)
{
	int err = liveupdate_register_file_handler(&eventfd_luo_handler);

	if (err && err != -EOPNOTSUPP) {
		pr_err("Could not register eventfd LUO handler: %pe\n",
		       ERR_PTR(err));
		return err;
	}

	return 0;
}
late_initcall(eventfd_luo_init);
