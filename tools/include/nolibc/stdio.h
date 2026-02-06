/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * minimal stdio function definitions for NOLIBC
 * Copyright (C) 2017-2021 Willy Tarreau <w@1wt.eu>
 */

/* make sure to include all global symbols */
#include "nolibc.h"

#ifndef _NOLIBC_STDIO_H
#define _NOLIBC_STDIO_H

#include "std.h"
#include "arch.h"
#include "errno.h"
#include "fcntl.h"
#include "types.h"
#include "sys.h"
#include "stdarg.h"
#include "stdlib.h"
#include "string.h"
#include "compiler.h"

static const char *strerror(int errnum);

#ifndef EOF
#define EOF (-1)
#endif

/* Buffering mode used by setvbuf.  */
#define _IOFBF 0	/* Fully buffered. */
#define _IOLBF 1	/* Line buffered. */
#define _IONBF 2	/* No buffering. */

/* just define FILE as a non-empty type. The value of the pointer gives
 * the FD: FILE=~fd for fd>=0 or NULL for fd<0. This way positive FILE
 * are immediately identified as abnormal entries (i.e. possible copies
 * of valid pointers to something else).
 */
typedef struct FILE {
	char dummy[1];
} FILE;

static __attribute__((unused)) FILE* const stdin  = (FILE*)(intptr_t)~STDIN_FILENO;
static __attribute__((unused)) FILE* const stdout = (FILE*)(intptr_t)~STDOUT_FILENO;
static __attribute__((unused)) FILE* const stderr = (FILE*)(intptr_t)~STDERR_FILENO;

/* provides a FILE* equivalent of fd. The mode is ignored. */
static __attribute__((unused))
FILE *fdopen(int fd, const char *mode __attribute__((unused)))
{
	if (fd < 0) {
		SET_ERRNO(EBADF);
		return NULL;
	}
	return (FILE*)(intptr_t)~fd;
}

static __attribute__((unused))
FILE *fopen(const char *pathname, const char *mode)
{
	int flags, fd;

	switch (*mode) {
	case 'r':
		flags = O_RDONLY;
		break;
	case 'w':
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	case 'a':
		flags = O_WRONLY | O_CREAT | O_APPEND;
		break;
	default:
		SET_ERRNO(EINVAL); return NULL;
	}

	if (mode[1] == '+')
		flags = (flags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;

	fd = open(pathname, flags, 0666);
	return fdopen(fd, mode);
}

/* provides the fd of stream. */
static __attribute__((unused))
int fileno(FILE *stream)
{
	intptr_t i = (intptr_t)stream;

	if (i >= 0) {
		SET_ERRNO(EBADF);
		return -1;
	}
	return ~i;
}

/* flush a stream. */
static __attribute__((unused))
int fflush(FILE *stream)
{
	intptr_t i = (intptr_t)stream;

	/* NULL is valid here. */
	if (i > 0) {
		SET_ERRNO(EBADF);
		return -1;
	}

	/* Don't do anything, nolibc does not support buffering. */
	return 0;
}

/* flush a stream. */
static __attribute__((unused))
int fclose(FILE *stream)
{
	intptr_t i = (intptr_t)stream;

	if (i >= 0) {
		SET_ERRNO(EBADF);
		return -1;
	}

	if (close(~i))
		return EOF;

	return 0;
}

/* getc(), fgetc(), getchar() */

#define getc(stream) fgetc(stream)

static __attribute__((unused))
int fgetc(FILE* stream)
{
	unsigned char ch;

	if (read(fileno(stream), &ch, 1) <= 0)
		return EOF;
	return ch;
}

static __attribute__((unused))
int getchar(void)
{
	return fgetc(stdin);
}


/* putc(), fputc(), putchar() */

#define putc(c, stream) fputc(c, stream)

static __attribute__((unused))
int fputc(int c, FILE* stream)
{
	unsigned char ch = c;

	if (write(fileno(stream), &ch, 1) <= 0)
		return EOF;
	return ch;
}

static __attribute__((unused))
int putchar(int c)
{
	return fputc(c, stdout);
}


/* fwrite(), fread(), puts(), fputs(). Note that puts() emits '\n' but not fputs(). */

/* internal fwrite()-like function which only takes a size and returns 0 on
 * success or EOF on error. It automatically retries on short writes.
 */
static __attribute__((unused))
int _fwrite(const void *buf, size_t size, FILE *stream)
{
	ssize_t ret;
	int fd = fileno(stream);

	while (size) {
		ret = write(fd, buf, size);
		if (ret <= 0)
			return EOF;
		size -= ret;
		buf += ret;
	}
	return 0;
}

static __attribute__((unused))
size_t fwrite(const void *s, size_t size, size_t nmemb, FILE *stream)
{
	size_t written;

	for (written = 0; written < nmemb; written++) {
		if (_fwrite(s, size, stream) != 0)
			break;
		s += size;
	}
	return written;
}

/* internal fread()-like function which only takes a size and returns 0 on
 * success or EOF on error. It automatically retries on short reads.
 */
static __attribute__((unused))
int _fread(void *buf, size_t size, FILE *stream)
{
	int fd = fileno(stream);
	ssize_t ret;

	while (size) {
		ret = read(fd, buf, size);
		if (ret <= 0)
			return EOF;
		size -= ret;
		buf += ret;
	}
	return 0;
}

static __attribute__((unused))
size_t fread(void *s, size_t size, size_t nmemb, FILE *stream)
{
	size_t nread;

	for (nread = 0; nread < nmemb; nread++) {
		if (_fread(s, size, stream) != 0)
			break;
		s += size;
	}
	return nread;
}

static __attribute__((unused))
int fputs(const char *s, FILE *stream)
{
	return _fwrite(s, strlen(s), stream);
}

static __attribute__((unused))
int puts(const char *s)
{
	if (fputs(s, stdout) == EOF)
		return EOF;
	return putchar('\n');
}


/* fgets() */
static __attribute__((unused))
char *fgets(char *s, int size, FILE *stream)
{
	int ofs;
	int c;

	for (ofs = 0; ofs + 1 < size;) {
		c = fgetc(stream);
		if (c == EOF)
			break;
		s[ofs++] = c;
		if (c == '\n')
			break;
	}
	if (ofs < size)
		s[ofs] = 0;
	return ofs ? s : NULL;
}


/* fseek */
static __attribute__((unused))
int fseek(FILE *stream, long offset, int whence)
{
	int fd = fileno(stream);
	off_t ret;

	ret = lseek(fd, offset, whence);

	/* lseek() and fseek() differ in that lseek returns the new
	 * position or -1, fseek() returns either 0 or -1.
	 */
	if (ret >= 0)
		return 0;

	return -1;
}


/* printf(). Supports most of the normal integer and string formats.
 *  - %[#-+ ][width][{l,t,z,ll,L,j,q}]{d,i,u,c,x,X,p,s,m,%}
 *  - %% generates a single %
 *  - %m outputs strerror(errno).
 *  - # only affects %x and prepends 0x to non-zero values.
 *  - %o (octal) isn't supported.
 *  - %X outputs a..f the same as %x.
 *  - No support for zero padding, precision or variable widths.
 *  - No support for wide characters.
 *  - invalid formats are copied to the output buffer.
 */

/* This code uses 'flag' variables that are indexed by the low 6 bits
 * of characters to optimise checks for multiple characters.
 *
 * _NOLIBC_PF_FLAGS_CONTAIN(flags, 'a', 'b'. ...)
 * returns non-zero if the bit for any of the specified characters is set.
 *
 * _NOLIBC_PF_CHAR_IS_ONE_OF(ch, 'a', 'b'. ...)
 * returns the flag bit for ch if it is one of the specified characters.
 * All the characters must be in the same 32 character block (non-alphabetic,
 * upper case, or lower case) of the ASCII character set.)
 */
#define _NOLIBC_PF_FLAG(ch) (1u << ((ch) & 0x1f))
#define _NOLIBC_PF_FLAG_NZ(ch) ((ch) ? _NOLIBC_PF_FLAG(ch) : 0)
#define _NOLIBC_PF_FLAG8(cmp_1, cmp_2, cmp_3, cmp_4, cmp_5, cmp_6, cmp_7, cmp_8, ...) \
	(_NOLIBC_PF_FLAG_NZ(cmp_1) | _NOLIBC_PF_FLAG_NZ(cmp_2) | \
	 _NOLIBC_PF_FLAG_NZ(cmp_3) | _NOLIBC_PF_FLAG_NZ(cmp_4) | \
	 _NOLIBC_PF_FLAG_NZ(cmp_5) | _NOLIBC_PF_FLAG_NZ(cmp_6) | \
	 _NOLIBC_PF_FLAG_NZ(cmp_7) | _NOLIBC_PF_FLAG_NZ(cmp_8))
#define _NOLIBC_PF_FLAGS_CONTAIN(flags, ...) \
	((flags) & _NOLIBC_PF_FLAG8(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0))
#define _NOLIBC_PF_CHAR_IS_ONE_OF(ch, cmp_1, ...) \
	(ch < (cmp_1 & ~0x1f) || ch > (cmp_1 | 0x1f) ? 0 : \
		_NOLIBC_PF_FLAGS_CONTAIN(_NOLIBC_PF_FLAG(ch), cmp_1, __VA_ARGS__))

typedef int (*__nolibc_printf_cb)(void *state, const char *buf, size_t size);

static __attribute__((unused, format(printf, 3, 0)))
int __nolibc_printf(__nolibc_printf_cb cb, void *state, const char *fmt, va_list args)
{
	char ch;
	unsigned int written, width;
	unsigned int flags, ch_flag;
	size_t len;
	char tmpbuf[32 + 24];
	const char *outstr;

	written = 0;
	while (1) {
		outstr = fmt;
		ch = *fmt++;
		if (!ch)
			break;

		width = 0;
		flags = 0;
		if (ch != '%') {
			while (*fmt && *fmt != '%')
				fmt++;
			len = fmt - outstr;
		} else {
			/* we're in a format sequence */

			ch = *fmt++;

			/* Conversion flag characters */
			for (;; ch = *fmt++) {
				ch_flag = _NOLIBC_PF_CHAR_IS_ONE_OF(ch, ' ', '#', '+', '-', '0');
				if (!ch_flag)
					break;
				flags |= ch_flag;
			}

			/* width */
			while (ch >= '0' && ch <= '9') {
				width *= 10;
				width += ch - '0';

				ch = *fmt++;
			}

			/* Length modifier.
			 * They miss the conversion flags characters " #+-0" so can go into flags.
			 * Change both L and ll to q.
			 */
			if (ch == 'L')
				ch = 'q';
			ch_flag = _NOLIBC_PF_CHAR_IS_ONE_OF(ch, 'l', 't', 'z', 'j', 'q');
			if (ch_flag != 0) {
				if (ch == 'l' && fmt[0] == 'l') {
					fmt++;
					ch_flag = _NOLIBC_PF_FLAG('q');
				}
				flags |= ch_flag;
				ch = *fmt++;
			}

			/* Conversion specifiers. */

			/* Numeric and pointer conversion specifiers.
			 *
			 * Use an explicit bound check (rather than _NOLIBC_PF_CHAR_IS_ONE_OF())
			 * so that 'X' can be allowed through.
			 * 'X' gets treated and 'x' because _NOLIBC_PF_FLAG() returns the same
			 * value for both.
			 */
			if ((ch < 'a' || ch > 'z') && ch != 'X')
				goto non_numeric_conversion;

			/* We need to check for "%p" or "%#x" later, merging here gives better code.
			 * But '#' collides with 'c' so shift right.
			 */
			ch_flag = _NOLIBC_PF_FLAG(ch) | (flags & _NOLIBC_PF_FLAG('#')) >> 1;
			if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 'c', 'd', 'i', 'u', 'x', 'p', 's')) {
				unsigned long long v;
				long long signed_v;
				char *out = tmpbuf + 32;
				int sign = 0;

				/* 'long' is needed for pointer/string conversions and ltz lengths.
				 * A single test can be used provided 'p' (the same bit as '0')
				 * is masked from flags.
				 */
				if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag | (flags & ~_NOLIBC_PF_FLAG('p')),
							     'p', 's', 'l', 't', 'z')) {
					v = va_arg(args, unsigned long);
					signed_v = (long)v;
				} else if (_NOLIBC_PF_FLAGS_CONTAIN(flags, 'j', 'q')) {
					v = va_arg(args, unsigned long long);
					signed_v = v;
				} else {
					v = va_arg(args, unsigned int);
					signed_v = (int)v;
				}

				if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 'c')) {
					/* "%c" - single character. */
					tmpbuf[0] = v;
					len = 1;
					outstr = tmpbuf;
					goto do_output;
				}

				if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 's')) {
					/* "%s" - character string. */
					if (!v) {
						outstr = "(null)";
						len = 6;
						goto do_output;
					}
					outstr = (void *)v;
do_strnlen_output:
					len = strnlen(outstr, INT_MAX);
					goto do_output;
				}

				if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 'd', 'i')) {
					/* "%d" and "%i" - signed decimal numbers. */
					if (signed_v < 0) {
						sign = '-';
						v = -(signed_v + 1);
						v++;
					} else if (_NOLIBC_PF_FLAGS_CONTAIN(flags, '+')) {
						sign = '+';
					} else if (_NOLIBC_PF_FLAGS_CONTAIN(flags, ' ')) {
						sign = ' ';
					}
				}

				/* Convert the number to ascii in the required base. */
				if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 'd', 'i', 'u')) {
					/* Base 10 */
					len = u64toa_r(v, out);
				} else {
					/* Base 16 */
					if (_NOLIBC_PF_FLAGS_CONTAIN(ch_flag, 'p', '#' - 1)) {
						/* "%p" and "%#x" need "0x" prepending. */
						sign = 'x' | '0' << 8;
					}
					len = u64toh_r(v, out);
				}

				/* Add 0, 1 or 2 ("0x") sign characters left of any zero padding */
				for (; sign; sign >>= 8) {
					len++;
					*--out = sign;
				}
				outstr = out;
				goto do_output;
			}

non_numeric_conversion:
			if (ch == 'm') {
#ifdef NOLIBC_IGNORE_ERRNO
				outstr = "unknown error";
				len = __builtin_strlen(outstr);
#else
				outstr = strerror(errno);
				goto do_strnlen_output;
#endif /* NOLIBC_IGNORE_ERRNO */
			} else {
				if (ch != '%') {
					/* Invalid format: back up to output the format characters */
					fmt = outstr + 1;
					/* and output a '%' now. */
				}
				/* %% is documented as a 'conversion specifier'.
				 * Any flags, precision or length modifier are ignored.
				 */
				len = 1;
				width = 0;
				outstr = fmt - 1;
			}
		}

do_output:
		written += len;

		/* An OPTIMIZER_HIDE_VAR() seems to stop gcc back-merging this
		 * code into one of the conditionals above.
		 */
		__asm__ volatile("" : "=r"(len) : "0"(len));

		/* Output 'left pad', 'value' then 'right pad'. */
		flags = _NOLIBC_PF_FLAGS_CONTAIN(flags, '-');
		if (flags && cb(state, outstr, len) != 0)
			return -1;
		while (width > len) {
			unsigned int pad_len = ((width - len - 1) & 15) + 1;
			width -= pad_len;
			written += pad_len;
			if (cb(state, "                ", pad_len) != 0)
				return -1;
		}
		if (!flags && cb(state, outstr, len) != 0)
			return -1;
	}

	/* Flush/terminate any buffer. */
	if (cb(state, NULL, 0) != 0)
		return -1;

	return written;
}

struct __nolibc_fprintf_cb_state {
	FILE *stream;
	unsigned int buf_offset;
	char buf[128];
};

static int __nolibc_fprintf_cb(void *v_state, const char *buf, size_t size)
{
	struct __nolibc_fprintf_cb_state *state = v_state;
	unsigned int off = state->buf_offset;

	if (off + size > sizeof(state->buf) || buf == NULL) {
		state->buf_offset = 0;
		if (off && _fwrite(state->buf, off, state->stream))
			return -1;
		if (size > sizeof(state->buf))
			return _fwrite(buf, size, state->stream);
		off = 0;
	}

	if (size) {
		state->buf_offset = off + size;
		memcpy(state->buf + off, buf, size);
	}
	return 0;
}

static __attribute__((unused, format(printf, 2, 0)))
int vfprintf(FILE *stream, const char *fmt, va_list args)
{
	struct __nolibc_fprintf_cb_state state;

	state.stream = stream;
	state.buf_offset = 0;
	return __nolibc_printf(__nolibc_fprintf_cb, &state, fmt, args);
}

static __attribute__((unused, format(printf, 1, 0)))
int vprintf(const char *fmt, va_list args)
{
	return vfprintf(stdout, fmt, args);
}

static __attribute__((unused, format(printf, 2, 3)))
int fprintf(FILE *stream, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vfprintf(stream, fmt, args);
	va_end(args);
	return ret;
}

static __attribute__((unused, format(printf, 1, 2)))
int printf(const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vfprintf(stdout, fmt, args);
	va_end(args);
	return ret;
}

static __attribute__((unused, format(printf, 2, 0)))
int vdprintf(int fd, const char *fmt, va_list args)
{
	FILE *stream;

	stream = fdopen(fd, NULL);
	if (!stream)
		return -1;
	/* Technically 'stream' is leaked, but as it's only a wrapper around 'fd' that is fine */
	return vfprintf(stream, fmt, args);
}

static __attribute__((unused, format(printf, 2, 3)))
int dprintf(int fd, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vdprintf(fd, fmt, args);
	va_end(args);

	return ret;
}

struct __nolibc_sprintf_cb_state {
	char *buf;
	size_t size;
};

static int __nolibc_sprintf_cb(void *v_state, const char *buf, size_t size)
{
	struct __nolibc_sprintf_cb_state *state = v_state;
	char *tgt;

	if (size >= state->size) {
		if (state->size <= 1)
			return 0;
		size = state->size - 1;
	}
	tgt = state->buf;
	if (size) {
		state->size -= size;
		state->buf = tgt + size;
		memcpy(tgt, buf, size);
	} else {
		/* In particular from cb(NULL, 0) at the end of __nolibc_printf(). */
		*tgt = '\0';
	}
	return 0;
}

static __attribute__((unused, format(printf, 3, 0)))
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	struct __nolibc_sprintf_cb_state state = { .buf = buf, .size = size };

	return __nolibc_printf(__nolibc_sprintf_cb, &state, fmt, args);
}

static __attribute__((unused, format(printf, 3, 4)))
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(buf, size, fmt, args);
	va_end(args);

	return ret;
}

static __attribute__((unused, format(printf, 2, 0)))
int vsprintf(char *buf, const char *fmt, va_list args)
{
	return vsnprintf(buf, SIZE_MAX, fmt, args);
}

static __attribute__((unused, format(printf, 2, 3)))
int sprintf(char *buf, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsprintf(buf, fmt, args);
	va_end(args);

	return ret;
}

static __attribute__((unused))
int vsscanf(const char *str, const char *format, va_list args)
{
	uintmax_t uval;
	intmax_t ival;
	int base;
	char *endptr;
	int matches;
	int lpref;

	matches = 0;

	while (1) {
		if (*format == '%') {
			/* start of pattern */
			lpref = 0;
			format++;

			if (*format == 'l') {
				/* same as in printf() */
				lpref = 1;
				format++;
				if (*format == 'l') {
					lpref = 2;
					format++;
				}
			}

			if (*format == '%') {
				/* literal % */
				if ('%' != *str)
					goto done;
				str++;
				format++;
				continue;
			} else if (*format == 'd') {
				ival = strtoll(str, &endptr, 10);
				if (lpref == 0)
					*va_arg(args, int *) = ival;
				else if (lpref == 1)
					*va_arg(args, long *) = ival;
				else if (lpref == 2)
					*va_arg(args, long long *) = ival;
			} else if (*format == 'u' || *format == 'x' || *format == 'X') {
				base = *format == 'u' ? 10 : 16;
				uval = strtoull(str, &endptr, base);
				if (lpref == 0)
					*va_arg(args, unsigned int *) = uval;
				else if (lpref == 1)
					*va_arg(args, unsigned long *) = uval;
				else if (lpref == 2)
					*va_arg(args, unsigned long long *) = uval;
			} else if (*format == 'p') {
				*va_arg(args, void **) = (void *)strtoul(str, &endptr, 16);
			} else {
				SET_ERRNO(EILSEQ);
				goto done;
			}

			format++;
			str = endptr;
			matches++;

		} else if (*format == '\0') {
			goto done;
		} else if (isspace(*format)) {
			/* skip spaces in format and str */
			while (isspace(*format))
				format++;
			while (isspace(*str))
				str++;
		} else if (*format == *str) {
			/* literal match */
			format++;
			str++;
		} else {
			if (!matches)
				matches = EOF;
			goto done;
		}
	}

done:
	return matches;
}

static __attribute__((unused, format(scanf, 2, 3)))
int sscanf(const char *str, const char *format, ...)
{
	va_list args;
	int ret;

	va_start(args, format);
	ret = vsscanf(str, format, args);
	va_end(args);
	return ret;
}

static __attribute__((unused))
void perror(const char *msg)
{
#ifdef NOLIBC_IGNORE_ERRNO
	fprintf(stderr, "%s%sunknown error\n", (msg && *msg) ? msg : "", (msg && *msg) ? ": " : "");
#else
	fprintf(stderr, "%s%serrno=%d\n", (msg && *msg) ? msg : "", (msg && *msg) ? ": " : "", errno);
#endif
}

static __attribute__((unused))
int setvbuf(FILE *stream __attribute__((unused)),
	    char *buf __attribute__((unused)),
	    int mode,
	    size_t size __attribute__((unused)))
{
	/*
	 * nolibc does not support buffering so this is a nop. Just check mode
	 * is valid as required by the spec.
	 */
	switch (mode) {
	case _IOFBF:
	case _IOLBF:
	case _IONBF:
		break;
	default:
		return EOF;
	}

	return 0;
}

static __attribute__((unused))
const char *strerror(int errno)
{
	static char buf[18] = "errno=";

	i64toa_r(errno, &buf[6]);

	return buf;
}

#endif /* _NOLIBC_STDIO_H */
