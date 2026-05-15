// SPDX-License-Identifier: GPL-2.0
/*
 * SECCOMP_IOCTL_NOTIF_INJECT: kernel-mode syscall execution on
 * behalf of an unprivileged seccomp_unotify supervisor.
 *
 * The supervisor describes a syscall (nr + args[6]) and a kernel-input
 * buffer; pointer-shaped args are encoded as byte offsets into that
 * buffer. On SECCOMP_USER_NOTIF_FLAG_INJECTED, the trapped task wakes
 * inside seccomp_do_user_notification(), dispatches into the matching
 * kernel-mode syscall helper, and the helper's return value becomes
 * the trapped syscall's return value. The trapped task's user mm is
 * never re-read for the substituted syscall, closing the documented
 * TOCTOU race against CLONE_VM peers.
 */
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <net/sock.h>
#include <uapi/linux/seccomp.h>
#include <asm/unistd.h>

#include "seccomp_inject.h"

/* Per-syscall injector. Runs in the trapped task's context. */
typedef long (*seccomp_injector_fn)(const struct seccomp_inject_record *rec);

static long inject_openat(const struct seccomp_inject_record *rec);
static long inject_bind(const struct seccomp_inject_record *rec);
static long inject_write(const struct seccomp_inject_record *rec);

/*
 * The injectable-syscall whitelist. Keep this small and explicit:
 * each entry pins a kernel-mode helper to a specific syscall number.
 * A NULL injector for an unlisted nr means "supervisor cannot inject
 * this syscall" and seccomp_inject_record_build() rejects it with
 * -EOPNOTSUPP.
 */
static seccomp_injector_fn seccomp_injector_for(u64 nr)
{
	switch (nr) {
#ifdef __NR_openat
	case __NR_openat:
		return inject_openat;
#endif
#ifdef __NR_bind
	case __NR_bind:
		return inject_bind;
#endif
#ifdef __NR_write
	case __NR_write:
		return inject_write;
#endif
	default:
		return NULL;
	}
}

/*
 * Treat args[i] as a byte offset into rec->buf and return a kernel
 * pointer at that offset, with @needed bytes required to be accessible
 * (i.e. offset + needed <= buf_size). NULL on out-of-bounds.
 */
static const void *inject_buf_at(const struct seccomp_inject_record *rec,
				 unsigned int i, size_t needed)
{
	u64 off;

	if (i >= 6)
		return NULL;
	if (!(rec->args_in_buf_mask & (1U << i)))
		return NULL;
	off = rec->args[i];
	if (off > rec->buf_size)
		return NULL;
	if (rec->buf_size - off < needed)
		return NULL;
	return (const u8 *)rec->buf + off;
}

/* NUL-terminated string at args[i] within rec->buf, or NULL on bound miss. */
static const char *inject_buf_cstr(const struct seccomp_inject_record *rec,
				   unsigned int i)
{
	const char *s;
	u64 off;
	size_t avail;

	if (i >= 6)
		return NULL;
	if (!(rec->args_in_buf_mask & (1U << i)))
		return NULL;
	off = rec->args[i];
	if (off >= rec->buf_size)
		return NULL;
	s = (const char *)rec->buf + off;
	avail = rec->buf_size - off;
	if (memchr(s, '\0', avail) == NULL)
		return NULL;
	return s;
}

static long inject_openat(const struct seccomp_inject_record *rec)
{
	int dfd = (int)rec->args[0];
	int flags = (int)rec->args[2];
	umode_t mode = (umode_t)rec->args[3];
	const char *path;
	struct file *f;
	int fd;

	/* v1: dfd must be AT_FDCWD. Other dfd values land in v2. */
	if (dfd != AT_FDCWD)
		return -EOPNOTSUPP;

	path = inject_buf_cstr(rec, 1);
	if (!path)
		return -EINVAL;

	f = filp_open(path, flags, mode);
	if (IS_ERR(f))
		return PTR_ERR(f);

	fd = get_unused_fd_flags(flags);
	if (fd < 0) {
		filp_close(f, NULL);
		return fd;
	}
	fd_install(fd, f);
	return fd;
}

static long inject_bind(const struct seccomp_inject_record *rec)
{
	int sockfd = (int)rec->args[0];
	int addrlen = (int)rec->args[2];
	struct sockaddr_storage addr;
	const void *src;
	struct socket *sock;
	long ret;
	int err;

	if (addrlen < 0 || addrlen > sizeof(addr))
		return -EINVAL;

	src = inject_buf_at(rec, 1, addrlen);
	if (!src)
		return -EINVAL;
	memcpy(&addr, src, addrlen);

	sock = sockfd_lookup(sockfd, &err);
	if (!sock)
		return err;

	ret = kernel_bind(sock, (struct sockaddr_unsized *)&addr, addrlen);
	sockfd_put(sock);
	return ret;
}

static long inject_write(const struct seccomp_inject_record *rec)
{
	int fd = (int)rec->args[0];
	size_t count = (size_t)rec->args[2];
	const void *src;
	ssize_t ret;
	struct fd f;
	loff_t pos;

	if (count > MAX_RW_COUNT)
		count = MAX_RW_COUNT;

	src = inject_buf_at(rec, 1, count);
	if (!src)
		return -EINVAL;

	f = fdget_pos(fd);
	if (!fd_file(f))
		return -EBADF;

	pos = fd_file(f)->f_pos;
	ret = kernel_write(fd_file(f), src, count, &pos);
	if (ret > 0)
		fd_file(f)->f_pos = pos;
	fdput_pos(f);
	return ret;
}

void seccomp_inject_record_free(struct seccomp_inject_record *rec)
{
	if (!rec)
		return;
	kvfree(rec->buf);
	kfree(rec);
}

/*
 * Validate @uinj fields (syscall_nr in whitelist, args_in_buf_mask
 * within bounds, offsets in bounds, buf_size capped), copy in the
 * supervisor's buf, and return a record the caller owns. Caller is
 * responsible for seccomp_inject_record_free() on success path if
 * the record is not subsequently attached to a knotif.
 */
long seccomp_inject_record_build(const struct seccomp_notif_inject *uinj,
				 struct seccomp_inject_record **out)
{
	struct seccomp_inject_record *rec;
	void __user *user_buf;
	long ret;
	unsigned int i;

	*out = NULL;

	if (!seccomp_injector_for(uinj->nr))
		return -EOPNOTSUPP;

	/* args_in_buf_mask must reference args[0..5] only. */
	if (uinj->args_in_buf_mask & ~((1U << 6) - 1))
		return -EINVAL;

	if (uinj->buf_size > SECCOMP_NOTIF_INJECT_MAX_BYTES)
		return -E2BIG;

	user_buf = (void __user *)(uintptr_t)uinj->buf;
	if (uinj->buf_size && !user_buf)
		return -EINVAL;

	/* Bounds-check each in-buf arg before any allocation. */
	for (i = 0; i < 6; i++) {
		if (!(uinj->args_in_buf_mask & (1U << i)))
			continue;
		if (uinj->args[i] > uinj->buf_size)
			return -EINVAL;
	}

	rec = kzalloc(sizeof(*rec), GFP_KERNEL_ACCOUNT);
	if (!rec)
		return -ENOMEM;

	rec->nr = uinj->nr;
	memcpy(rec->args, uinj->args, sizeof(rec->args));
	rec->args_in_buf_mask = uinj->args_in_buf_mask;
	rec->buf_size = uinj->buf_size;

	if (uinj->buf_size) {
		rec->buf = kvmalloc(uinj->buf_size, GFP_KERNEL_ACCOUNT);
		if (!rec->buf) {
			ret = -ENOMEM;
			goto err;
		}
		if (copy_from_user(rec->buf, user_buf, uinj->buf_size)) {
			ret = -EFAULT;
			goto err;
		}
	}

	*out = rec;
	return 0;

err:
	seccomp_inject_record_free(rec);
	return ret;
}

/*
 * Top-level dispatch. Runs in the trapped task's context (current is
 * the trapped task). Returns the kernel helper's result.
 */
long seccomp_inject_dispatch(const struct seccomp_inject_record *rec)
{
	seccomp_injector_fn injector = seccomp_injector_for(rec->nr);

	if (!injector)
		return -EOPNOTSUPP;
	return injector(rec);
}
