/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wrapper functions for accessing the file_struct fd array.
 */

#ifndef __LINUX_FILE_H
#define __LINUX_FILE_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/posix_types.h>
#include <linux/errno.h>
#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/vfsdebug.h>

struct file;

extern void fput(struct file *);

struct file_operations;
struct task_struct;
struct vfsmount;
struct dentry;
struct inode;
struct path;
extern struct file *alloc_file_pseudo(struct inode *, struct vfsmount *,
	const char *, int flags, const struct file_operations *);
extern struct file *alloc_file_pseudo_noaccount(struct inode *, struct vfsmount *,
	const char *, int flags, const struct file_operations *);
extern struct file *alloc_file_clone(struct file *, int flags,
	const struct file_operations *);

/* either a reference to struct file + flags
 * (cloned vs. borrowed, pos locked), with
 * flags stored in lower bits of value,
 * or empty (represented by 0).
 */
struct fd {
	unsigned long word;
};
#define FDPUT_FPUT       1
#define FDPUT_POS_UNLOCK 2

#define fd_file(f) ((struct file *)((f).word & ~(FDPUT_FPUT|FDPUT_POS_UNLOCK)))
static inline bool fd_empty(struct fd f)
{
	return unlikely(!f.word);
}

#define EMPTY_FD (struct fd){0}
static inline struct fd BORROWED_FD(struct file *f)
{
	return (struct fd){(unsigned long)f};
}
static inline struct fd CLONED_FD(struct file *f)
{
	return (struct fd){(unsigned long)f | FDPUT_FPUT};
}

static inline void fdput(struct fd fd)
{
	if (unlikely(fd.word & FDPUT_FPUT))
		fput(fd_file(fd));
}

extern struct file *fget(unsigned int fd);
extern struct file *fget_raw(unsigned int fd);
extern struct file *fget_task(struct task_struct *task, unsigned int fd);
extern struct file *fget_task_next(struct task_struct *task, unsigned int *fd);
extern void __f_unlock_pos(struct file *);

struct fd fdget(unsigned int fd);
struct fd fdget_raw(unsigned int fd);
struct fd fdget_pos(unsigned int fd);

static inline void fdput_pos(struct fd f)
{
	if (f.word & FDPUT_POS_UNLOCK)
		__f_unlock_pos(fd_file(f));
	fdput(f);
}

DEFINE_CLASS(fd, struct fd, fdput(_T), fdget(fd), int fd)
DEFINE_CLASS(fd_raw, struct fd, fdput(_T), fdget_raw(fd), int fd)
DEFINE_CLASS(fd_pos, struct fd, fdput_pos(_T), fdget_pos(fd), int fd)

extern int f_dupfd(unsigned int from, struct file *file, unsigned flags);
extern int replace_fd(unsigned fd, struct file *file, unsigned flags);
extern void set_close_on_exec(unsigned int fd, int flag);
extern bool get_close_on_exec(unsigned int fd);
extern int __get_unused_fd_flags(unsigned flags, unsigned long nofile);
extern int get_unused_fd_flags(unsigned flags);
extern void put_unused_fd(unsigned int fd);

DEFINE_CLASS(get_unused_fd, int, if (_T >= 0) put_unused_fd(_T),
	     get_unused_fd_flags(flags), unsigned flags)
DEFINE_FREE(fput, struct file *, if (!IS_ERR_OR_NULL(_T)) fput(_T))

/*
 * take_fd() will take care to set @fd to -EBADF ensuring that
 * CLASS(get_unused_fd) won't call put_unused_fd(). This makes it
 * easier to rely on CLASS(get_unused_fd):
 *
 * struct file *f;
 *
 * CLASS(get_unused_fd, fd)(O_CLOEXEC);
 * if (fd < 0)
 *         return fd;
 *
 * f = dentry_open(&path, O_RDONLY, current_cred());
 * if (IS_ERR(f))
 *         return PTR_ERR(f);
 *
 * fd_install(fd, f);
 * return take_fd(fd);
 */
#define take_fd(fd) __get_and_null(fd, -EBADF)

extern void fd_install(unsigned int fd, struct file *file);

int receive_fd(struct file *file, int __user *ufd, unsigned int o_flags);

int receive_fd_replace(int new_fd, struct file *file, unsigned int o_flags);

extern void flush_delayed_fput(void);
extern void __fput_sync(struct file *);

extern unsigned int sysctl_nr_open_min, sysctl_nr_open_max;

/*
 * fd_prepare class: Combined fd + file allocation with automatic cleanup.
 *
 * Allocates an fd and a file together. On error paths, automatically cleans
 * up whichever resource was successfully allocated. Allows flexible file
 * allocation with different functions per usage.
 */

struct fd_prepare {
	struct file *file;
	int fd;
};

/*
 * fd_prepare_fd() - Get fd from fd_prepare structure
 * @fd_prepare: struct fd_prepare to extract fd from
 *
 * Returns the file descriptor from an fd_prepare structure.
 *
 * Return: The file descriptor
 */
static inline int fd_prepare_fd(struct fd_prepare fdp)
{
	return fdp.fd;
}

/*
 * fd_prepare_file() - Get file from fd_prepare structure
 * @fd_prepare: struct fd_prepare to extract file from
 *
 * Returns the file pointer from an fd_prepare structure.
 *
 * Return: The file pointer
 */
static inline struct file *fd_prepare_file(struct fd_prepare fdp)
{
	return fdp.file;
}

/*
 * fd_prepare_failed() - Check if fd_prepare allocation failed
 * @fd_prepare: struct fd_prepare to check
 *
 * Checks whether either the fd allocation or file allocation failed.
 *
 * Return: true if either allocation failed, false otherwise
 */
static inline bool fd_prepare_failed(struct fd_prepare fdp)
{
	VFS_WARN_ON_ONCE(fdp.fd < 0 && IS_ERR(fdp.file));
	return fdp.fd < 0 || IS_ERR(fdp.file);
}

/*
 * fd_prepare_error() - Get error from failed fd_prepare
 * @fd_prepare: struct fd_prepare to extract error from
 *
 * Returns the error code from the first allocation that failed.
 * Should only be called after fd_prepare_failed() returns true.
 *
 * Return: Negative error code
 */
static inline int fd_prepare_error(struct fd_prepare fdp)
{
	if (fdp.fd < 0) {
		VFS_WARN_ON_ONCE(fdp.file);
		return fdp.fd;
	}
	if (!fdp.file)
		return -ENOMEM;
	return PTR_ERR(fdp.file);
}

static __always_inline void __fd_prepare_put(struct fd_prepare fdp)
{
	if (unlikely(fdp.fd >= 0))
		put_unused_fd(fdp.fd);
	if (unlikely(!IS_ERR_OR_NULL(fdp.file)))
		fput(fdp.file);
}

DEFINE_CLASS_TYPE(fd_prepare, struct fd_prepare, __fd_prepare_put(_T))

/*
 * __FD_PREPARE_INIT(fd_flags, file_init_expr):
 *     Helper to initialize fd_prepare class.
 *     @fd_flags: flags for get_unused_fd_flags()
 *     @file_init_expr: expression that returns struct file *
 *
 * Returns a struct fd_prepare with fd and file set.
 * If fd allocation fails, file will be NULL.
 * If fd succeeds but file_init_expr fails, fd will be cleaned up.
 */
#define __FD_PREPARE_INIT(_fd_flags, _file_init_owned)          \
	({                                                      \
		struct fd_prepare _fd_prepare = {               \
			.fd = get_unused_fd_flags((_fd_flags)), \
		};                                              \
		if (likely(_fd_prepare.fd >= 0))                \
			_fd_prepare.file = (_file_init_owned);  \
		_fd_prepare;                                    \
	})

/*
 * FD_PREPARE(var, fd_flags, file_init_owned):
 *     Convenience wrapper for CLASS_INIT(fd_prepare, ...).
 *
 * @_var: name of struct fd_prepare variable to define
 * @_fd_flags: flags for get_unused_fd_flags()
 * @_file_init_owned: struct file to take ownership of (can be expression)
 *
 * Ex.
 * FD_PREPARE(fdf, O_RDWR | O_CLOEXEC,
 *                  anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep, O_RDWR));
 * if (fd_prepare_failed(fdf))
 *     return fd_prepare_error(fdf);
 *
 * ep->file = fd_prepare_file(fdf);
 * return fd_publish(fdf);
 *
 * Or with different file init function:
 *
 * FD_PREPARE(fdf, flags,
 *                  anon_inode_getfile_fmode("[eventfd]", &eventfd_fops, ctx, flags, FMODE_NOWAIT));
 * if (fd_prepare_failed(fdf))
 *     return fd_prepare_error(fdf);
 *
 * return fd_publish(fdf);
 */
#define FD_PREPARE(_var, _fd_flags, _file_init_owned) \
	scoped_class_init(fd_prepare, _var, __FD_PREPARE_INIT(_fd_flags, _file_init_owned))

#define fd_publish(_fd_prepare)                          \
	({                                               \
		struct fd_prepare *__p = &(_fd_prepare); \
		fd_install(__p->fd, __p->file);          \
		retain_and_null_ptr(__p->file);          \
		take_fd(__p->fd);                        \
	})

#endif /* __LINUX_FILE_H */
