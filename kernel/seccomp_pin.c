// SPDX-License-Identifier: GPL-2.0
/*
 * Pin-args lifecycle and walker for SECCOMP_IOCTL_NOTIF_PIN_ARGS.
 *
 * The supervisor calls PIN_ARGS to atomically copy designated pointer-arg
 * payloads of a trapped child into kernel-owned buffers, then sends
 * NOTIF_SEND with CONTINUE | CONTINUE_PINNED. The kernel re-executes the
 * syscall using the pinned bytes instead of re-reading user memory,
 * closing the documented seccomp_unotify(2) TOCTOU race.
 *
 * The lock-and-validate dance lives in kernel/seccomp.c (where
 * struct seccomp_knotif and filter->notify_lock are defined). This file
 * owns the per-arg walker (Phase B) and the lifecycle primitives.
 *
 * Only SECCOMP_PIN_FIXED is implemented in v1's first cut; CSTRING and
 * CSTRING_ARRAY arrive in subsequent patches.
 */
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task_stack.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#include <asm/syscall.h>

#include "seccomp_pin.h"

struct seccomp_pinned_args *seccomp_alloc_pinned_args(u8 nr_args)
{
	struct seccomp_pinned_args *kpa;

	if (nr_args == 0 || nr_args > SECCOMP_PIN_MAX_ARGS)
		return ERR_PTR(-EINVAL);

	kpa = kzalloc_obj(*kpa, GFP_KERNEL_ACCOUNT);
	if (!kpa)
		return ERR_PTR(-ENOMEM);
	kpa->nr_args = nr_args;
	return kpa;
}

void seccomp_free_pinned_args(struct seccomp_pinned_args *kpa)
{
	int i;

	if (!kpa)
		return;
	for (i = 0; i < kpa->nr_args; i++)
		kvfree(kpa->args[i].data);
	kfree(kpa);
}

void seccomp_clear_pinned_args(struct task_struct *task)
{
	struct seccomp_pinned_args *kpa;

	/*
	 * Atomically claim ownership of the kpa: this can be called
	 * concurrently from the task's own task_work callback (returning
	 * to userspace after a CONTINUE_PINNED'd syscall), from a
	 * listener-release path on the supervisor side, and from task
	 * exit. Only the xchg winner frees.
	 */
	kpa = xchg(&task->seccomp.pinned_args, NULL);
	if (!kpa)
		return;
	/*
	 * Cancel any queued post-syscall clear; its callback_head lives
	 * inside @kpa and would otherwise dangle. If task_work_cancel
	 * returns false the callback has already started running on @task,
	 * but it does its work via current->seccomp.pinned_args (already
	 * NULL) so the in-flight callback observes nothing-to-do.
	 */
	if (kpa->clear_queued)
		task_work_cancel(task, &kpa->clear_work);
	seccomp_free_pinned_args(kpa);
}

/*
 * task_work callback: runs on the trapped task when it returns to user
 * mode after the resumed syscall body has completed. The pin is single-
 * shot; subsequent traps must call PIN_ARGS again.
 */
static void seccomp_pin_clear_cb(struct callback_head *cb)
{
	seccomp_clear_pinned_args(current);
}

int seccomp_pin_queue_clear(struct task_struct *task)
{
	struct seccomp_pinned_args *kpa = task->seccomp.pinned_args;
	int ret;

	if (!kpa || kpa->clear_queued)
		return 0;
	init_task_work(&kpa->clear_work, seccomp_pin_clear_cb);
	ret = task_work_add(task, &kpa->clear_work, TWA_RESUME);
	if (ret == 0)
		kpa->clear_queued = true;
	return ret;
}

/* Snapshot SECCOMP_PIN_FIXED: copy exactly @desc->max_bytes from @user_addr
 * in the trapped child's mm into a freshly-allocated kernel buffer.
 *
 * On success, @out is populated and @desc->actual_size / .truncated are
 * filled. The caller is responsible for chaining the bytes into the
 * supervisor's bulk buffer.
 */
static long pin_one_fixed(struct task_struct *task, u64 user_addr,
			  struct seccomp_pin_arg *desc,
			  struct seccomp_pinned_arg *out)
{
	struct mm_struct *mm;
	void *kbuf;
	int read;

	kbuf = kvmalloc(desc->max_bytes, GFP_KERNEL_ACCOUNT);
	if (!kbuf)
		return -ENOMEM;

	mm = get_task_mm(task);
	if (!mm) {
		kvfree(kbuf);
		return -ESRCH;
	}

	read = access_remote_vm(mm, user_addr, kbuf, desc->max_bytes, 0);
	mmput(mm);

	if (read <= 0) {
		kvfree(kbuf);
		return read ? read : -EFAULT;
	}

	out->user_addr = user_addr;
	out->size      = read;
	out->arg_idx   = desc->arg_idx;
	out->kind      = SECCOMP_PIN_FIXED;
	out->data      = kbuf;

	desc->actual_size = read;
	desc->truncated   = (read < desc->max_bytes) ?
				SECCOMP_PIN_TRUNCATED_BYTES : 0;
	return 0;
}

/* MAX_ARG_STRINGS is fs/exec.c-private; redefine our own ceiling. */
#define SECCOMP_PIN_DEFAULT_MAX_ENTRIES	0x7FFFFFFF

/*
 * Packed CSTRING_ARRAY layout:
 *
 *   [u32 count][u32 offsets[count]][u8 strings[]]
 *
 * Each offset is from the start of the buffer; each string at
 * data + offsets[i] is NUL-terminated.
 */

/* Snapshot SECCOMP_PIN_CSTRING: NUL-bounded copy from the trapped child's
 * mm via the existing copy_remote_vm_str() primitive. The result is
 * always NUL-terminated; truncation is reported when the byte cap was
 * hit before the source NUL.
 */
static long pin_one_cstring(struct task_struct *task, u64 user_addr,
			    struct seccomp_pin_arg *desc,
			    struct seccomp_pinned_arg *out)
{
	void *kbuf;
	int copied;

	kbuf = kvmalloc(desc->max_bytes, GFP_KERNEL_ACCOUNT);
	if (!kbuf)
		return -ENOMEM;

	copied = copy_remote_vm_str(task, user_addr, kbuf, desc->max_bytes, 0);
	if (copied < 0) {
		kvfree(kbuf);
		return copied;
	}

	/*
	 * copy_remote_vm_str() returns bytes not including the trailing NUL,
	 * which it always writes on success. If we filled the buffer all the
	 * way (copied == max_bytes - 1) the source NUL may not have been
	 * reached; flag that as truncation.
	 */
	out->user_addr = user_addr;
	out->size      = copied + 1;	/* include the trailing NUL */
	out->arg_idx   = desc->arg_idx;
	out->kind      = SECCOMP_PIN_CSTRING;
	out->data      = kbuf;

	desc->actual_size = copied + 1;
	desc->truncated   = (copied == desc->max_bytes - 1) ?
				SECCOMP_PIN_TRUNCATED_BYTES : 0;
	return 0;
}

/*
 * Snapshot SECCOMP_PIN_CSTRING_ARRAY: walk the NULL-terminated pointer
 * table at @user_addr in the trapped child's mm; for each non-NULL ptr,
 * copy its NUL-bounded string into a packed kernel buffer. Format:
 *
 *   [u32 count][u32 offsets[count]][u8 strings[]]
 *
 * Caps on both byte total (@desc->max_bytes) and entry count
 * (@desc->max_entries; 0 means default cap). The pointer table is
 * walked first to determine count, *before* any string copy, so a
 * hostile child can't tie up the kernel walking a giant table.
 *
 * v1: native pointer width only. Compat (32-bit pointer table read by
 * a native supervisor) is a TODO.
 */
static long pin_one_cstring_array(struct task_struct *task, u64 user_addr,
				  struct seccomp_pin_arg *desc,
				  struct seccomp_pinned_arg *out)
{
	struct mm_struct *mm;
	void *kbuf = NULL;
	u32 max_entries;
	u32 *header;
	u32 count = 0;
	u32 byte_off;
	u32 truncated = 0;
	u32 i;
	long ret;

	max_entries = desc->max_entries ?: SECCOMP_PIN_DEFAULT_MAX_ENTRIES;
	/* Cap entries by what fits in the supervisor's max_bytes assuming
	 * even the smallest header (count + per-entry offset + 1 NUL).
	 * Each entry costs at least 4 (offset) + 1 (NUL) = 5 bytes.
	 */
	if (max_entries > (desc->max_bytes / 5))
		max_entries = desc->max_bytes / 5;

	if (desc->max_bytes < sizeof(u32))
		return -EINVAL;

	kbuf = kvmalloc(desc->max_bytes, GFP_KERNEL_ACCOUNT);
	if (!kbuf)
		return -ENOMEM;

	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto err_free;
	}

	/* Phase 1: count entries by walking the pointer table. */
	for (i = 0; i < max_entries; i++) {
		unsigned long ptr;
		int got;

		got = access_remote_vm(mm, user_addr + i * sizeof(ptr),
				       &ptr, sizeof(ptr), 0);
		if (got != sizeof(ptr)) {
			mmput(mm);
			ret = -EFAULT;
			goto err_free;
		}
		if (ptr == 0)
			break;
		count++;
	}
	if (i == max_entries) {
		/* Hit the entry cap before the NULL terminator: still report
		 * what we have, flag truncation.
		 */
		truncated |= SECCOMP_PIN_TRUNCATED_ENTRIES;
	}

	/* Header layout fits in max_bytes? */
	if ((u64)sizeof(u32) + (u64)count * sizeof(u32) > desc->max_bytes) {
		mmput(mm);
		ret = -EINVAL;
		goto err_free;
	}

	header = kbuf;
	header[0] = count;
	byte_off = sizeof(u32) + count * sizeof(u32);

	/* Phase 2: copy each string into the packed area. */
	for (i = 0; i < count; i++) {
		unsigned long ptr;
		u32 remaining;
		int got, copied;

		if (access_remote_vm(mm, user_addr + i * sizeof(ptr),
				     &ptr, sizeof(ptr), 0) != sizeof(ptr)) {
			mmput(mm);
			ret = -EFAULT;
			goto err_free;
		}
		if (byte_off >= desc->max_bytes) {
			truncated |= SECCOMP_PIN_TRUNCATED_BYTES;
			count = i;
			header[0] = count;
			break;
		}
		remaining = desc->max_bytes - byte_off;
		copied = copy_remote_vm_str(task, ptr,
					    (char *)kbuf + byte_off,
					    remaining, 0);
		if (copied < 0) {
			mmput(mm);
			ret = copied;
			goto err_free;
		}
		header[1 + i] = byte_off;
		got = copied + 1;	/* include the NUL written by helper */
		if (got >= remaining)
			truncated |= SECCOMP_PIN_TRUNCATED_BYTES;
		byte_off += got;
	}
	mmput(mm);

	out->user_addr = user_addr;
	out->size      = byte_off;
	out->arg_idx   = desc->arg_idx;
	out->kind      = SECCOMP_PIN_CSTRING_ARRAY;
	out->data      = kbuf;

	desc->actual_size    = byte_off;
	desc->actual_entries = count;
	desc->truncated      = truncated;
	return 0;

err_free:
	kvfree(kbuf);
	return ret;
}

const struct kvec *seccomp_pin_kvec_for(const struct seccomp_pinned_arg *pin)
{
	struct seccomp_pinned_args *kpa;
	long idx;

	kpa = READ_ONCE(current->seccomp.pinned_args);
	if (!kpa)
		return NULL;
	idx = pin - kpa->args;
	if (idx < 0 || idx >= kpa->nr_args)
		return NULL;
	return &kpa->arg_kvecs[idx];
}

const struct seccomp_pinned_arg *seccomp_pin_lookup_current(u64 user_addr)
{
	struct seccomp_pinned_args *kpa;
	int i;

	kpa = READ_ONCE(current->seccomp.pinned_args);
	if (!kpa || !kpa->live)
		return NULL;

	/*
	 * If the current syscall doesn't match the one snapshotted at pin
	 * time, return NULL so the caller reads user memory. This guards
	 * against a signal handler issuing an unrelated syscall during
	 * -ERESTART* resolution — that syscall has its own user pointers
	 * and must not be served from the pin.
	 */
	if (kpa->syscall_nr !=
	    syscall_get_nr(current, task_pt_regs(current)))
		return NULL;

	for (i = 0; i < kpa->nr_args; i++) {
		if (kpa->args[i].user_addr == user_addr)
			return &kpa->args[i];
	}
	return NULL;
}

long seccomp_pin_args_walk(struct task_struct *task,
			   struct seccomp_notif_pin_args *kargs,
			   const u64 *args, int syscall_nr,
			   void __user *user_buf, u32 user_buf_size,
			   struct seccomp_pinned_args **out)
{
	struct seccomp_pinned_args *kpa;
	u32 buf_off = 0;
	int i;
	long ret;

	kpa = seccomp_alloc_pinned_args(kargs->nr_args);
	if (IS_ERR(kpa))
		return PTR_ERR(kpa);
	kpa->notif_id   = kargs->id;
	kpa->syscall_nr = syscall_nr;

	for (i = 0; i < kargs->nr_args; i++) {
		struct seccomp_pin_arg *d = &kargs->args[i];
		u64 user_addr = args[d->arg_idx];

		d->user_addr      = user_addr;
		d->actual_size    = 0;
		d->actual_entries = 0;
		d->truncated      = 0;
		d->buf_offset     = buf_off;

		/* NULL pointers (e.g. execveat with AT_EMPTY_PATH): record
		 * a zero-size pin and move on without faulting.
		 */
		if (user_addr == 0)
			continue;

		switch (d->kind) {
		case SECCOMP_PIN_FIXED:
			ret = pin_one_fixed(task, user_addr, d, &kpa->args[i]);
			break;
		case SECCOMP_PIN_CSTRING:
			ret = pin_one_cstring(task, user_addr, d, &kpa->args[i]);
			break;
		case SECCOMP_PIN_CSTRING_ARRAY:
			ret = pin_one_cstring_array(task, user_addr, d,
						    &kpa->args[i]);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		if (ret < 0)
			goto err_free;

		/* Stable kvec for iov_iter_kvec consumers (import_ubuf). */
		kpa->arg_kvecs[i].iov_base = kpa->args[i].data;
		kpa->arg_kvecs[i].iov_len  = kpa->args[i].size;

		if (kpa->args[i].size > user_buf_size - buf_off) {
			ret = -ENOSPC;
			goto err_free;
		}
		if (copy_to_user(user_buf + buf_off,
				 kpa->args[i].data, kpa->args[i].size)) {
			ret = -EFAULT;
			goto err_free;
		}
		d->buf_offset = buf_off;
		buf_off += kpa->args[i].size;
	}

	*out = kpa;
	return 0;

err_free:
	seccomp_free_pinned_args(kpa);
	return ret;
}
