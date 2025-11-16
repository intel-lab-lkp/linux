/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#ifndef __ILITEK_DEF_H__
#define __ILITEK_DEF_H__

#define COMMONFLOW_CODE_VERSION		0x00000301

/*
 * Windows
 */
#ifdef _WIN32
#include <windows.h>
#include <time.h>
#include <fcntl.h>
#include <io.h>

#define __MAYBE_UNUSED

#ifdef _WINDLL
#define __DLL __declspec(dllexport)
#else
#define __DLL __declspec(dllimport)
#endif

#define __PACKED__

#define _sprintf(buf, idx, fmt, ...)	\
	sprintf_s((buf) + (idx), sizeof((buf)) - (idx), (fmt), ##__VA_ARGS__)
#define _strncpy(dst, src, n, dst_size)	strncpy_s((dst), (dst_size), (src), (n))
#define _strcasecmp(l, r)		_stricmp((l), (r))
#define _strcat(dst, src, dst_size)	strcat_s((dst), (dst_size), (src))
#define _sscanf(str, fmt, ...)		sscanf_s(str, fmt, ##__VA_ARGS__)
#define _strlen(str)			strlen((str))
#define _strcpy(dst, src, dst_size)	strcpy_s((dst), (dst_size), (src))
#define _strtok(str, del, next_token)	strtok_s((str), (del), (next_token))

#define _memset(ptr, ch, size)		memset((ptr), (ch), (size))
#define _memcpy(dst, src, size)		memcpy((dst), (src), (size))

#define _WTEXT(str)			L ## str
#define WTEXT(str)			_WTEXT(str)
#define WCHAR				wchar_t
#define WSTRING				wstring
#define WCSCPY(dst, src, dst_size)	wcscpy_s((dst), (dst_size), (src))
#define WCSCASECMP(str, tag)		_wcsicmp((str), (L##tag))
#define WCSRCHR(str, ch)		wcsrchr((str), (ch))
#define SWPRINTF(buf, size, fmt, ...)	\
	swprintf_s((buf), (size), (fmt), ##__VA_ARGS__)
#define WFOPEN(pfp, filename, mode)					\
	((!((*(pfp)) = _wfsopen((filename), (const wchar_t *)(L##mode), \
				_SH_DENYWR))) ? -EFAULT : 0)
#define WACCESS(filename, mode)		_waccess((filename), (mode))
#define WFPRINTF(fp, fmt, ...)					\
	do {							\
		fflush((fp));					\
		_setmode(_fileno((fp)), _O_U8TEXT);		\
		fwprintf((fp), (fmt), ##__VA_ARGS__);		\
		fflush((fp));					\
		_setmode(_fileno((fp)), _O_TEXT);		\
	} while (false)

#define PFMT_C16			"%ls"
#define PFMT_C8				"%hs"

#include <codecvt>
#define TO_WCHAR(x)	\
	std::wstring_convert < std::codecvt_utf8 < wchar_t >, wchar_t > ().from_bytes(x).c_str()

#define _localtime(ptm, ptime)		localtime_s((ptm), (ptime))

#define _fopen(pfp, filename, mode)	\
	(fopen_s((pfp), (filename), (const char *)(mode)))
#define _fclose(pfp)			fclose((pfp))

#define MUTEX_T				HANDLE
#define MUTEX_INIT(x)			((x) = CreateMutex(NULL, false, NULL))
#define MUTEX_LOCK(x)			(WaitForSingleObject((x), INFINITE))
#define MUTEX_UNLOCK(x)			(ReleaseMutex((x)))
#define MUTEX_EXIT(x)			(CloseHandle((x)))

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MALLOC(size)		malloc(size)
#define CALLOC(num, size)	calloc(num, size)
#define FREE(ptr)		\
	do {			\
		free((ptr));	\
		(ptr) = NULL;	\
	} while (0)
#define CFREE(ptr)		FREE(ptr)

/*
 * UEFI Driver
 */
#elif defined(__UEFI_DXE__)
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>

#define UEFI_MAX_STR_SIZE	0x1000

#define __MAYBE_UNUSED	__maybe_unused
#define __DLL
#define __PACKED__	__packed

#define _sprintf(buf, idx, fmt, ...)	\
	AsciiSPrint((buf) + (idx), sizeof((buf)) - (idx), (fmt), ##__VA_ARGS__)
#define _strncpy(dst, src, n, dst_size) \
	AsciiStrnCpyS(dst, dst_size, src, (UINTN)n)
#define _strcasecmp(l, r)		AsciiStriCmp((l), (r))
#define _strcat(dst, src, dst_size)	\
	AsciiStrCatS((dst), (dst_size), (src))
#define _sscanf(str, fmt, ...)
#define _strlen(str)			AsciiStrnLenS((str), UEFI_MAX_STR_SIZE)
#define _strcpy(dst, src, dst_size)	\
	_strncpy((dst), (src), _strlen(src), (dst_size))
#define _strtok(str, del, next_token)

static __MAYBE_UNUSED char *_strrchr(const char *s, char c)
{
	char *found = NULL;

	do {
		if (*s == c)
			found = (char *)s;
	} while (*s++ != '\0');

	return found;
}

#define _memset(ptr, ch, size)		\
	SetMem((ptr), (UINTN)(size), (UINT8)(ch))
#define _memcpy(dst, src, size)		\
	CopyMem((dst), (src), (UINTN)(size))

#define _WTEXT(str)			L ## str
#define WTEXT(str)			_WTEXT(str)
#define WCHAR				char
#define WSTRING				string
#define WCSCPY(dst, src, dst_size)	_strcpy((dst), (src), (dst_size))
#define WCSCASECMP(str, tag)		_strcasecmp((str), (tag))
#define WCSRCHR(str, ch)		_strrchr((str), (ch))
#define SWPRINTF(buf, size, fmt, ...)	\
	sprintf((buf), (fmt), ##__VA_ARGS__)
#define WFOPEN(pfp, filename, mode)	(-EINVAL)
#define WACCESS(filename, mode)		(-EINVAL)
#define WFPRINTF(fp, fmt, ...)
#define TO_WCHAR(x)			(x)

#define PFMT_C16			"%a"
#define PFMT_C8				"%a"
#define PFMT_U8				"%u"
#define PFMT_U16			"%u"
#define PFMT_X8				"%02x"
#define PFMT_X16			"%04x"
#define PFMT_X64			"%016llx"

#define _localtime(ptm, ptime)
#define _fclose(pfp)

#define MUTEX_T				void *
#define MUTEX_INIT(x)
#define MUTEX_LOCK(x)
#define MUTEX_UNLOCK(x)
#define MUTEX_EXIT(x)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

#define MALLOC(size)		AllocatePool(size)
#define CALLOC(num, size)	AllocateZeroPool(num * size)
#define FREE(ptr)			\
	do {				\
		FreePool((ptr));	\
		(ptr) = NULL;		\
	} while (0)

#define CFREE(ptr)		FREE(ptr)
#define TP_PRINTF(fmt, ...)	DebugPrint(DEBUG_INFO, fmt, ##__VA_ARGS__)
#define TP_LOG(fp, str)

/*
 * Linux-Based System
 */
#else

#define __MAYBE_UNUSED	__maybe_unused
#define __DLL
#define __PACKED__	__packed
#define _sprintf(buf, idx, fmt, ...)	sprintf((buf) + (idx), (fmt), ##__VA_ARGS__)
#define _strncpy(dst, src, n, dst_size) strncpy((dst), (src), (n))
#define _strcasecmp(l, r)		strcasecmp((l), (r))
#define _strcat(dst, src, dst_size)	strcat((dst), (src))
#define _sscanf(str, fmt, ...)		sscanf(str, fmt, ##__VA_ARGS__)
#define _strlen(str)			strlen((str))
#define _strcpy(dst, src, dst_size)	strcpy((dst), (src))

#define _memset(ptr, ch, size)		memset((ptr), (ch), (size))
#define _memcpy(dst, src, size)		memcpy((dst), (src), (size))

#define WTEXT(str)			str
#define WCHAR				char
#define WSTRING				string
#define WCSCPY(dst, src, dst_size)	_strcpy((dst), (src), (dst_size))
#define WCSCASECMP(str, tag)		_strcasecmp((str), (tag))
#define WCSRCHR(str, ch)		strrchr((str), (ch))
#define SWPRINTF(buf, size, fmt, ...)	\
	sprintf((buf), (fmt), ##__VA_ARGS__)
#define WFOPEN(pfp, filename, mode)	_fopen(pfp, filename, mode)
#define WACCESS(filename, mode)		_access((filename), (mode))
#define WFPRINTF(fp, fmt, ...)		fprintf((fp), (fmt), ##__VA_ARGS__)
#define TO_WCHAR(x)			(x)

#define PFMT_C16			"%s"
#define PFMT_C8				"%s"

#define _localtime(ptm, ptime)						\
		do {							\
			struct tm *__tm__;				\
									\
			__tm__ = localtime((ptime));			\
			_memcpy((ptm), __tm__, sizeof(struct tm));	\
		} while (false)

#ifdef __KERNEL__
#define FILE				void
#define _fopen(pfp, filename, mode)	(-EINVAL)
#define _fclose(pfp)
#define _access(filename, mode)		(-EINVAL)

#define _strtok(str, del, next_token)

#include <linux/spinlock.h>
#define MUTEX_T				spinlock_t
#define MUTEX_INIT(x)			spin_lock_init(&(x))
#define MUTEX_LOCK(x)			spin_lock(&(x))
#define MUTEX_UNLOCK(x)			spin_unlock(&(x))
#define MUTEX_EXIT(x)

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#define MALLOC(size)		kmalloc(size, GFP_KERNEL)
#define CALLOC(num, size)	vmalloc(num * size)
#define FREE(ptr)		\
	do {			\
		kfree((ptr));	\
		(ptr) = NULL;	\
	} while (0)
#define CFREE(ptr)		\
	do {			\
		vfree((ptr));	\
		(ptr) = NULL;	\
	} while (0)

#define TP_PRINTF(fmt, ...)	printk(fmt, ##__VA_ARGS__)
#define TP_LOG(fp, str)

#else
#include <sys/time.h>
#include <unistd.h>

#define _fopen(pfp, filename, mode)	\
	((!((*(pfp)) = fopen((filename), (mode)))) ? -EFAULT : 0)
#define _fclose(pfp)			fclose((pfp))
#define _access(filename, mode)		access((filename), (mode))

#define _strtok(str, del, next_token)	strtok((str), (del))

#ifdef __UEFI_APP__
#define MUTEX_T				void *
#define MUTEX_INIT(x)
#define MUTEX_LOCK(x)
#define MUTEX_UNLOCK(x)
#define MUTEX_EXIT(x)
#else
#include <pthread.h>
#define MUTEX_T				pthread_mutex_t
#define MUTEX_INIT(x)			(pthread_mutex_init(&(x), NULL))
#define MUTEX_LOCK(x)			(pthread_mutex_lock(&(x)))
#define MUTEX_UNLOCK(x)			(pthread_mutex_unlock(&(x)))
#define MUTEX_EXIT(x)			(pthread_mutex_destroy(&(x)))
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MALLOC(size)		malloc(size)
#define CALLOC(num, size)	calloc(num, size)
#define FREE(ptr)		\
	do {			\
		free((ptr));	\
		(ptr) = NULL;	\
	} while (0)
#define CFREE(ptr)		FREE(ptr)

#ifdef __ANDROID__
#include <android/log.h>
#define TP_PRINTF(fmt, ...)							\
	__android_log_print(ANDROID_LOG_INFO, "ILITEK COMMON", "[%s][%d]" fmt,	\
			    __func__, __LINE__, ##__VA_ARGS__)
#endif

#endif /* __KERNEL__ */

#endif /* _WIN32 */

#define U82U64(byte, order)	\
	((u64)((u64)(byte) << ((order) * 8)))

#ifndef PFMT_C16
#define PFMT_C16			"%ls"
#endif
#ifndef PFMT_C8
#define PFMT_C8				"%hs"
#endif
#ifndef PFMT_U8
#define PFMT_U8				"%hhu"
#endif
#ifndef PFMT_U16
#define PFMT_U16			"%hu"
#endif
#ifndef PFMT_X8
#define PFMT_X8				"%hhx"
#endif
#ifndef PFMT_X16
#define PFMT_X16			"%hx"
#endif

#ifndef PFMT_X64
#define PFMT_X64			"%llx"
#endif

#ifndef TP_PRINTF
#define TP_PRINTF(fmt, ...)				\
	do {						\
		printf(fmt, ##__VA_ARGS__);		\
		fflush(stdout);				\
	} while (0)
#endif

#ifndef TP_LOG
#define TP_LOG(fp, str)				\
	do {					\
		if (!fp)			\
			break;			\
		fprintf((fp), PFMT_C8, (str));	\
		fflush((fp));			\
	} while (0)
#endif

#ifndef TP_PRINT
#define TP_PRINT(_id, level, need_tag, tag, fmt, ...)			\
	do {								\
		char *__id__ = (_id);					\
		u32 __time_ms__;					\
									\
		if (level > tp_log_level)				\
			break;						\
									\
		g_str[0] = '\0';					\
									\
		if (need_tag) {						\
			if (!get_time_ms(&__time_ms__))			\
				_sprintf(g_str, _strlen(g_str),		\
					"[%7u.%03u]",			\
					__time_ms__ / 1000,		\
					__time_ms__ % 1000);		\
			_sprintf(g_str, _strlen(g_str), PFMT_C8, tag);	\
		}							\
									\
		if (__id__) {						\
			_sprintf(g_str, _strlen(g_str),			\
				 "[" PFMT_C8 "] " fmt,			\
				 __id__, ##__VA_ARGS__);		\
		} else {						\
			_sprintf(g_str, _strlen(g_str), " " fmt,	\
				 ##__VA_ARGS__);			\
		}							\
									\
		if (tp_print_en)					\
			TP_PRINTF(PFMT_C8, g_str);			\
		if (g_msg)						\
			g_msg(level, g_str);				\
		TP_LOG(tp_fp, g_str);					\
	} while (0)
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif

#ifndef UNUSED
#define UNUSED(x)		((void)(x))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)		((sizeof(a) / sizeof(*(a))))
#endif

#ifndef MIN
#define MIN(l, r)		(((l) > (r)) ? (r) : (l))
#endif

#ifndef MAX
#define MAX(l, r)		(((l) > (r)) ? (l) : (r))
#endif

#define EILICOMM		200
#define EILIBUSY		201
#define EILITIME		202
#define EILIPROTO		203

enum ilitek_log_level {
	log_level_none = -1,	/* no log displayed */
	log_level_err = 0,	/* critical errors */
	log_level_warn,		/* warnings */
	log_level_tag,		/* special-required tags */
	log_level_info,		/* important/UI messages */
	log_level_msg,		/* non-important messages */
	log_level_dbg,		/* debugging messages */
	log_level_pkt,		/* tx/rx packets */

	log_level_max,		/* sentinel */
};

#define _TP_ERR(fmt, ...)	\
	TP_PRINT(NULL, log_level_err, false, "", fmt, ##__VA_ARGS__)
#define _TP_WARN(fmt, ...)	\
	TP_PRINT(NULL, log_level_warn, false, "", fmt, ##__VA_ARGS__)
#define _TP_TAG(fmt, ...)	\
	TP_PRINT(NULL, log_level_tag, false, "", fmt, ##__VA_ARGS__)
#define _TP_INFO(fmt, ...)	\
	TP_PRINT(NULL, log_level_info, false, "", fmt, ##__VA_ARGS__)
#define _TP_MSG(fmt, ...)	\
	TP_PRINT(NULL, log_level_msg, false, "", fmt, ##__VA_ARGS__)
#define _TP_DBG(fmt, ...)	\
	TP_PRINT(NULL, log_level_dbg, false, "", fmt, ##__VA_ARGS__)
#define _TP_PKT(fmt, ...)	\
	TP_PRINT(NULL, log_level_pkt, false, "", fmt, ##__VA_ARGS__)


#define TP_ERR(id, fmt, ...)	\
	TP_PRINT(id, log_level_err, true, "[ILITEK][ERR]", fmt, ##__VA_ARGS__)
#define TP_WARN(id, fmt, ...)	\
	TP_PRINT(id, log_level_warn, true, "[ILITEK][WARN]", fmt, ##__VA_ARGS__)
#define TP_TAG(id, fmt, ...)	\
	TP_PRINT(id, log_level_tag, true, "[ILITEK][TAG]", fmt, ##__VA_ARGS__)
#define TP_INFO(id, fmt, ...)	\
	TP_PRINT(id, log_level_info, true, "[ILITEK][INFO]", fmt, ##__VA_ARGS__)
#define TP_MSG(id, fmt, ...)	\
	TP_PRINT(id, log_level_msg, true, "[ILITEK][MSG]", fmt, ##__VA_ARGS__)
#define TP_DBG(id, fmt, ...)	\
	TP_PRINT(id, log_level_dbg, true, "[ILITEK][DBG]", fmt, ##__VA_ARGS__)
#define TP_PKT(id, fmt, ...)	\
	TP_PRINT(id, log_level_pkt, true, "[ILITEK][PKT]", fmt, ##__VA_ARGS__)

enum ilitek_array_type {
	TYPE_U8 = 0,
	TYPE_INT,
	TYPE_U32,
};

#define TP_ERR_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_err, "[ILITEK][ERR]", tag, type, len, buf)
#define TP_WARN_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_warn, "[ILITEK][WARN]", tag, type, len, buf)
#define TP_TAG_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_tag, "[ILITEK][TAG]", tag, type, len, buf)
#define TP_INFO_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_info, "[ILITEK][INFO]", tag, type, len, buf)
#define TP_MSG_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_msg, "[ILITEK][MSG]", tag, type, len, buf)
#define TP_DBG_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_dbg, "[ILITEK][DBG]", tag, type, len, buf)
#define TP_PKT_ARR(id, tag, type, len, buf) \
	tp_log_arr(id, log_level_pkt, "[ILITEK][PKT]", tag, type, len, buf)

extern int tp_log_level;
extern bool tp_print_en;
extern char g_str[4096];
extern FILE *tp_fp;

typedef void (*msg_t)(int, char *);
extern msg_t g_msg;

struct queue {
	u32 curr_size;
	u32 max_size;
	u8 *buf;
	u8 *push_ptr;
	u8 *pop_ptr;
	u8 *end_ptr;
	u32 item_size;

	MUTEX_T mutex;
};

#ifdef __cplusplus
extern "C" {
#endif

void __DLL tp_log_arr(char *id, int level, const char *header,
		      const char *tag, int type, int len, void *buf);

int __DLL get_time_ms(u32 *t_ms);
void __DLL set_print_en(bool enable);
void __DLL set_log_level(int level);
int __DLL set_log_fopen(WCHAR *filename);
void __DLL set_log_fclose(void);
void __DLL set_log_fwrite(char *str);

int queue_init(struct queue *q, u32 item_size, u32 max_items);
void queue_exit(struct queue *q);
void queue_push(struct queue *q);
void queue_pop(struct queue *q);

#ifdef __cplusplus
}
#endif

#endif
