/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MIPS UHI semihosting define and helpers
 *
 * Based on MD01069 - Unified Hosting Interface Reference Manual
 *
 * Copyright (C) 2023 Jiaxun Yang <jiaxun.yang@flygoat.com>
 */


#ifndef _MIPS_UHI_H_
#define _MIPS_UHI_H_

#include <linux/compiler.h>
#include <linux/types.h>

#define UHI_NR_exit		1
#define UHI_NR_open		2
#define UHI_NR_close		3
#define UHI_NR_read		4
#define UHI_NR_write		5
#define UHI_NR_lseek		6
#define UHI_NR_unlink		7
#define UHI_NR_fstat		8
#define UHI_NR_argc		9
#define UHI_NR_argnlen		10
#define UHI_NR_argn		11
#define UHI_NR_plog		13
#define UHI_NR_assert		14
#define UHI_NR_pread		19
#define UHI_NR_pwrite		20
#define UHI_NR_link		22
#define UHI_NR_bootfailure	23

/* For SDDBP code and R2 */
#define UHI_CALL_CODE		"1"
#define UHI_CALL_CODE_RAW	1

struct uhi_stat {
	int16_t st_dev;
	uint16_t st_ino;
	uint32_t st_mode;
	uint16_t st_nlink;
	uint16_t st_uid;
	uint16_t st_gid;
	int16_t st_rdev;
	uint64_t st_size;
	uint64_t st_atime;
	uint64_t st_spare1;
	uint64_t st_mtime;
	uint64_t st_spare2;
	uint64_t st_ctime;
	uint64_t st_spare3;
	uint64_t st_blksize;
	uint64_t st_blocks;
	uint64_t st_spare4[2];
};

#define UHI_O_RDONLY	0x0
#define UHI_O_WRONLY	0x1
#define UHI_O_RDWR	0x2
#define UHI_O_APPEND	0x8
#define UHI_O_CREAT	0x200
#define UHI_O_TRUNC	0x400
#define UHI_O_EXCL	0x800

#define UHI_EACCESS		13
#define UHI_EAGAIN		11
#define UHI_EBADF		9
#define UHI_EBADMSG		77
#define UHI_EBUSY		16
#define UHI_ECONNRESET		104
#define UHI_EEXIST		17
#define UHI_EFBIG		27
#define UHI_EINTR		4
#define UHI_EINVAL		22
#define UHI_EIO			5
#define UHI_EISDIR		21
#define UHI_ELOOP		92
#define UHI_EMFILE		24
#define UHI_EMLINK		31
#define UHI_ENAMETOOLONG	91
#define UHI_ENETDOWN		115
#define UHI_ENETUNREACH		114
#define UHI_ENFILE		23
#define UHI_ENOBUFS		105
#define UHI_ENOENT		2
#define UHI_ENOMEM		12
#define UHI_ENOSPC		28
#define UHI_ENOSR		63
#define UHI_ENOTCONN		128
#define UHI_ENOTDIR		20
#define UHI_ENXIO		6
#define UHI_EOVERFLOW		139
#define UHI_EPERM		1
#define UHI_EPIPE		32
#define UHI_ERANGE		34
#define UHI_EROFS		30
#define UHI_ESPIPE		29
#define UHI_ETIMEDOUT		116
#define UHI_ETXTBSY		26
#define UHI_EWOULDBLOCK		11
#define UHI_EXDEV		18


struct uhi_gpctx {
	unsigned long r[31];
	unsigned long epc;
	unsigned long badvaddr;
	unsigned long hi;
	unsigned long lo;
	void *link;
	uint32_t status;
	uint32_t cause;
	uint32_t badinstr;
	uint32_t badpinstr;
};

static inline long __uhi_call0(long n)
{
	register long r2 __asm__("$2");
	register long r25 __asm__("$25") = n;
	register long r3 __asm__("$3");

	__asm__ __volatile__ (
		".set	push   \n"
		".set	" MIPS_ISA_LEVEL " \n"
		"addu $2,$0,%2 \n"
		"sdbbp " UHI_CALL_CODE "   \n"
		".set	pop	\n"
		: "=&r"(r2), "=r"(r3)
		: "ir"(UHI_CALL_CODE_RAW), "0"(r2), "r"(r25)
		: "memory");

	return r2 == -1 ? -r3 : r2;
}

static inline long __uhi_call1(long n, long a)
{
	register long r2 __asm__("$2");
	register long r25 __asm__("$25") = n;
	register long r4 __asm__("$4") = a;
	register long r3 __asm__("$3");

	__asm__ __volatile__ (
		".set	push   \n"
		".set	" MIPS_ISA_LEVEL " \n"
		"addu $2,$0,%2 \n"
		"sdbbp " UHI_CALL_CODE "   \n"
		".set	pop	\n"
		: "=&r"(r2), "=r"(r3)
		: "ir"(UHI_CALL_CODE_RAW), "0"(r2), "r"(r25), "r"(r4)
		: "memory");

	return r2 == -1 ? -r3 : r2;
}

static inline long __uhi_call2(long n, long a, long b)
{
	register long r2 __asm__("$2");
	register long r25 __asm__("$25") = n;
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r3 __asm__("$3");

	__asm__ __volatile__ (
		".set	push   \n"
		".set	" MIPS_ISA_LEVEL " \n"
		"addu $2,$0,%2 \n"
		"sdbbp " UHI_CALL_CODE "   \n"
		".set	pop	\n"
		: "=&r"(r2), "=r"(r3)
		: "ir"(UHI_CALL_CODE_RAW), "0"(r2), "r"(r25), "r"(r4), "r"(r5)
		: "memory");

	return r2 == -1 ? -r3 : r2;
}

static inline long __uhi_call3(long n, long a, long b, long c)
{
	register long r2 __asm__("$2");
	register long r25 __asm__("$25") = n;
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r3 __asm__("$3");

	__asm__ __volatile__ (
		".set	push   \n"
		".set	" MIPS_ISA_LEVEL " \n"
		"addu $2,$0,%2 \n"
		"sdbbp " UHI_CALL_CODE "   \n"
		".set	pop	\n"
		: "=&r"(r2), "=r"(r3)
		: "ir"(UHI_CALL_CODE_RAW), "0"(r2), "r"(r25), "r"(r4), "r"(r5), "r"(r6)
		: "memory");

	return r2 == -1 ? -r3 : r2;
}

static inline long __uhi_call4(long n, long a, long b, long c, long d)
{
	register long r2 __asm__("$2");
	register long r25 __asm__("$25") = n;
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7") = d;
	register long r3 __asm__("$3");

	__asm__ __volatile__ (
		".set	push   \n"
		".set	" MIPS_ISA_LEVEL " \n"
		"addu $2,$0,%2 \n"
		"sdbbp " UHI_CALL_CODE "   \n"
		".set	pop	\n"
		: "=&r"(r2), "=r"(r3)
		: "ir"(UHI_CALL_CODE_RAW), "0"(r2), "r"(r25), "r"(r4), "r"(r5), "r"(r6), "r"(r7)
		: "memory");

	return r2 == -1 ? -r3 : r2;
}

static inline int32_t uhi_exit(int32_t code)
{
	return __uhi_call1(UHI_NR_exit, code);
}

static inline int32_t uhi_open(const char *path, int32_t flags, int32_t mode)
{
	return __uhi_call3(UHI_NR_open, (long)path, flags, mode);
}

static inline int32_t uhi_close(int32_t fd)
{
	return __uhi_call1(UHI_NR_close, fd);
}

static inline int32_t uhi_read(int32_t fd, char *buf, long num)
{
	return __uhi_call3(UHI_NR_read, fd, (long)buf, num);
}

static inline long uhi_write(int32_t fd, const char *buf, long num)
{
	return __uhi_call3(UHI_NR_write, fd, (long)buf, num);
}

static inline int32_t uhi_lseek(int32_t fd, long offset, int32_t whence)
{
	return __uhi_call3(UHI_NR_lseek, fd, offset, whence);
}

static inline int32_t uhi_unlink(const char *path)
{
	return __uhi_call1(UHI_NR_unlink, (long)path);
}

static inline int32_t uhi_fstat(int32_t fd, struct uhi_stat *buf)
{
	return __uhi_call2(UHI_NR_fstat, fd, (long)buf);
}

static inline int32_t uhi_argc(void)
{
	return __uhi_call0(UHI_NR_argc);
}

static inline int32_t uhi_argnlen(int32_t n)
{
	return __uhi_call1(UHI_NR_argnlen, n);
}

static inline int32_t uhi_argn(int32_t n, char *buf, int32_t num)
{
	return __uhi_call3(UHI_NR_argn, n, (long)buf, num);
}

static inline int32_t uhi_plog(const char *buf, int32_t num)
{
	return __uhi_call2(UHI_NR_plog, (long)buf, num);
}

static inline void uhi_assert(const char *msg, const char *file, int32_t line)
{
	__uhi_call3(UHI_NR_assert, (long)msg, (long)file, line);
}

static inline int32_t uhi_link(const char *oldpath, const char *newpath)
{
	return __uhi_call2(UHI_NR_link, (long)oldpath, (long)newpath);
}

static inline int32_t uhi_pread(int32_t fd, char *buf, long num, long offset)
{
	return __uhi_call4(UHI_NR_pread, fd, (long)buf, num, offset);
}

static inline int32_t uhi_pwrite(int32_t fd, const char *buf, long num, long offset)
{
	return __uhi_call4(UHI_NR_pwrite, fd, (long)buf, num, offset);
}

static inline void uhi_bootfailure(int reason)
{
	__uhi_call1(UHI_NR_bootfailure, reason);
}

#endif
