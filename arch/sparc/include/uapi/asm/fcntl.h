/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _SPARC_FCNTL_H
#define _SPARC_FCNTL_H

#define O_APPEND	0000000010
#define FASYNC		0000000100	/* fcntl, for BSD compatibility */
#define O_CREAT		0000001000	/* not fcntl */
#define O_TRUNC		0000002000	/* not fcntl */
#define O_EXCL		0000004000	/* not fcntl */
#define O_DSYNC		0000020000	/* used to be O_SYNC, see below */
#define O_NONBLOCK	0000040000
#if defined(__sparc__) && defined(__arch64__)
#define O_NDELAY	0000000004
#else
#define O_NDELAY	(0000000004 | O_NONBLOCK)
#endif
#define O_NOCTTY	0000100000	/* not fcntl */
#define O_LARGEFILE	0001000000
#define O_DIRECT        0004000000 /* direct disk access hint */
#define O_NOATIME	0010000000
#define O_CLOEXEC	0020000000
/*
 * Before Linux 2.6.33 only O_DSYNC semantics were implemented, but using
 * the O_SYNC flag.  We continue to use the existing numerical value
 * for O_DSYNC semantics now, but using the correct symbolic name for it.
 * This new value is used to request true Posix O_SYNC semantics.  It is
 * defined in this strange way to make sure applications compiled against
 * new headers get at least O_DSYNC semantics on older kernels.
 *
 * This has the nice side-effect that we can simply test for O_DSYNC
 * wherever we do not care if O_DSYNC or O_SYNC is used.
 *
 * Note: __O_SYNC must never be used directly.
 */
#define __O_SYNC	0040000000
#define O_SYNC		(__O_SYNC|O_DSYNC)

#define O_PATH		0100000000
#define __O_TMPFILE	0200000000
#define OPENAT2_REGULAR	0400000000

#define F_GETOWN	5	/*  for sockets. */
#define F_SETOWN	6	/*  for sockets. */
#define F_GETLK		7
#define F_SETLK		8
#define F_SETLKW	9

/* for posix fcntl() and lockf() */
#define F_RDLCK		1
#define F_WRLCK		2
#define F_UNLCK		3

#define __ARCH_FLOCK_PAD	short __unused;
#define __ARCH_FLOCK64_PAD	short __unused;

#include <asm-generic/fcntl.h>

#endif
