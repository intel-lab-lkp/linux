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
void __fd_slots_commit(long ret);
struct pt_regs;
void fd_slots_commit(struct pt_regs *regs);
void exit_fd_slots(void);

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

struct fd_slot;
const struct fd_slot *fd_prepare(unsigned flags);
int fd_stage(const struct fd_slot *slot, struct file *file);
int fd_prepare_fd(const struct fd_slot *slot);
struct file *fd_prepare_file(const struct fd_slot *slot);

int receive_fd(struct file *file, int __user *ufd, unsigned int o_flags);

int receive_fd_replace(int new_fd, struct file *file, unsigned int o_flags);

extern void flush_delayed_fput(void);
extern void __fput_sync(struct file *);

extern unsigned int sysctl_nr_open_min, sysctl_nr_open_max;

/*
 * FD_ADD - allocate a descriptor, build the file and install it in one step.
 * @_fd_flags: flags for get_unused_fd_flags()
 * @_file_owned: struct file to take ownership of (can be an expression)
 *
 * The file expression is evaluated only after the descriptor is allocated, so
 * a full table does not run its side effects. Drops the file and returns a
 * negative errno on failure. Installs immediately: for anything more than a
 * bare install-and-return (reporting the number, configuring the file,
 * installing several descriptors) use fd_prepare()/fd_stage().
 */
#define FD_ADD(_fd_flags, _file_owned)					\
({									\
	int __fd = get_unused_fd_flags(_fd_flags);			\
	if (likely(__fd >= 0)) {					\
		struct file *__file = (_file_owned);			\
		if (unlikely(IS_ERR_OR_NULL(__file))) {			\
			put_unused_fd(__fd);				\
			__fd = __file ? PTR_ERR(__file) : -ENOMEM;	\
		} else {						\
			fd_install(__fd, __file);			\
		}							\
	}								\
	__fd;								\
})

/*
 * FD_PREPARE - reserve a descriptor and stage @_file_owned on it for the
 * install at syscall exit; declares @_fdf, an fd_prepare() slot.
 * @_fdf: name of the const struct fd_slot * to declare
 * @_fd_flags: flags for get_unused_fd_flags()
 * @_file_owned: struct file to take ownership of (can be an expression)
 */
#define FD_PREPARE(_fdf, _fd_flags, _file_owned)			\
	const struct fd_slot *_fdf = fd_prepare(_fd_flags);		\
	if (!IS_ERR(_fdf)) {						\
		struct file *__file = (_file_owned);			\
		if (unlikely(IS_ERR_OR_NULL(__file)))			\
			_fdf = __file ? ERR_CAST(__file) : ERR_PTR(-ENOMEM); \
		else							\
			fd_stage(_fdf, __file);				\
	}

#endif /* __LINUX_FILE_H */
