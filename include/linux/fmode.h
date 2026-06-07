/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FMODE_H
#define _LINUX_FMODE_H

/*
 * flags in file.f_mode.  Note that FMODE_READ and FMODE_WRITE must correspond
 * to O_WRONLY and O_RDWR via the strange trick in do_dentry_open()
 */

/* file is open for reading */
#define FMODE_READ		((__force fmode_t)(1 << 0))
/* file is open for writing */
#define FMODE_WRITE		((__force fmode_t)(1 << 1))
/* file is seekable */
#define FMODE_LSEEK		((__force fmode_t)(1 << 2))
/* file can be accessed using pread */
#define FMODE_PREAD		((__force fmode_t)(1 << 3))
/* file can be accessed using pwrite */
#define FMODE_PWRITE		((__force fmode_t)(1 << 4))
/* File is opened for execution with sys_execve / sys_uselib */
#define FMODE_EXEC		((__force fmode_t)(1 << 5))
/* File writes are restricted (block device specific) */
#define FMODE_WRITE_RESTRICTED	((__force fmode_t)(1 << 6))
/* File supports atomic writes */
#define FMODE_CAN_ATOMIC_WRITE	((__force fmode_t)(1 << 7))

/* FMODE_* bit 8 */

/* 32bit hashes as llseek() offset (for directories) */
#define FMODE_32BITHASH         ((__force fmode_t)(1 << 9))
/* 64bit hashes as llseek() offset (for directories) */
#define FMODE_64BITHASH         ((__force fmode_t)(1 << 10))

/*
 * Don't update ctime and mtime.
 *
 * Currently a special hack for the XFS open_by_handle ioctl, but we'll
 * hopefully graduate it to a proper O_CMTIME flag supported by open(2) soon.
 */
#define FMODE_NOCMTIME		((__force fmode_t)(1 << 11))

/* Expect random access pattern */
#define FMODE_RANDOM		((__force fmode_t)(1 << 12))

/* Supports IOCB_HAS_METADATA */
#define FMODE_HAS_METADATA	((__force fmode_t)(1 << 13))

/* File is opened with O_PATH; almost nothing can be done with it */
#define FMODE_PATH		((__force fmode_t)(1 << 14))

/* File needs atomic accesses to f_pos */
#define FMODE_ATOMIC_POS	((__force fmode_t)(1 << 15))
/* Write access to underlying fs */
#define FMODE_WRITER		((__force fmode_t)(1 << 16))
/* Has read method(s) */
#define FMODE_CAN_READ          ((__force fmode_t)(1 << 17))
/* Has write method(s) */
#define FMODE_CAN_WRITE         ((__force fmode_t)(1 << 18))

#define FMODE_OPENED		((__force fmode_t)(1 << 19))
#define FMODE_CREATED		((__force fmode_t)(1 << 20))

/* File is stream-like */
#define FMODE_STREAM		((__force fmode_t)(1 << 21))

/* File supports DIRECT IO */
#define	FMODE_CAN_ODIRECT	((__force fmode_t)(1 << 22))

#define	FMODE_NOREUSE		((__force fmode_t)(1 << 23))

/* File is embedded in backing_file object */
#define FMODE_BACKING		((__force fmode_t)(1 << 24))

/*
 * Together with FMODE_NONOTIFY_PERM defines which fsnotify events shouldn't be
 * generated (see below)
 */
#define FMODE_NONOTIFY		((__force fmode_t)(1 << 25))

/*
 * Together with FMODE_NONOTIFY defines which fsnotify events shouldn't be
 * generated (see below)
 */
#define FMODE_NONOTIFY_PERM	((__force fmode_t)(1 << 26))

/* File is capable of returning -EAGAIN if I/O will block */
#define FMODE_NOWAIT		((__force fmode_t)(1 << 27))

/* File represents mount that needs unmounting */
#define FMODE_NEED_UNMOUNT	((__force fmode_t)(1 << 28))

/* File does not contribute to nr_files count */
#define FMODE_NOACCOUNT		((__force fmode_t)(1 << 29))

/*
 * The two FMODE_NONOTIFY* define which fsnotify events should not be generated
 * for an open file. These are the possible values of
 * (f->f_mode & FMODE_FSNOTIFY_MASK) and their meaning:
 *
 * FMODE_NONOTIFY - suppress all (incl. non-permission) events.
 * FMODE_NONOTIFY_PERM - suppress permission (incl. pre-content) events.
 * FMODE_NONOTIFY | FMODE_NONOTIFY_PERM - suppress only FAN_ACCESS_PERM.
 */
#define FMODE_FSNOTIFY_MASK \
	(FMODE_NONOTIFY | FMODE_NONOTIFY_PERM)

#define FMODE_FSNOTIFY_NONE(mode) \
	((mode & FMODE_FSNOTIFY_MASK) == FMODE_NONOTIFY)
#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
#define FMODE_FSNOTIFY_HSM(mode) \
	((mode & FMODE_FSNOTIFY_MASK) == 0 || \
	 (mode & FMODE_FSNOTIFY_MASK) == (FMODE_NONOTIFY | FMODE_NONOTIFY_PERM))
#define FMODE_FSNOTIFY_ACCESS_PERM(mode) \
	((mode & FMODE_FSNOTIFY_MASK) == 0)
#else
#define FMODE_FSNOTIFY_ACCESS_PERM(mode) 0
#define FMODE_FSNOTIFY_HSM(mode)	0
#endif

#endif /* _LINUX_FMODE_H */
