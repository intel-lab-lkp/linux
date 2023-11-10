/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CMDLINE_H
#define _LINUX_CMDLINE_H
/*
 *
 * Copyright (C) 2006,2021. Cisco Systems, Inc.
 *
 * Generic Append/Prepend cmdline support.
 */


#include <linux/ctype.h>
#include <linux/cache.h>
#include <asm/setup.h>

#ifdef CONFIG_CMDLINE_BOOL
extern char cmdline_prepend[];
extern char cmdline_append[];
extern char cmdline_tmp[];
#define CMDLINE_PREPEND cmdline_prepend
#define CMDLINE_APPEND cmdline_append
#define CMDLINE_TMP cmdline_tmp
#define CMDLINE_STATIC_PREPEND CONFIG_CMDLINE_PREPEND
#define CMDLINE_STATIC_APPEND CONFIG_CMDLINE_APPEND
#else
#define CMDLINE_PREPEND ""
#define CMDLINE_APPEND ""
#define CMDLINE_TMP ""
#define CMDLINE_STATIC_PREPEND ""
#define CMDLINE_STATIC_APPEND ""
#endif

#ifndef CMDLINE_STRLCAT
#define CMDLINE_STRLCAT strlcat
#endif

#ifndef CMDLINE_STRLEN
#define CMDLINE_STRLEN strlen
#endif

/*
 * This function will append or prepend a builtin command line to the command
 * line provided by the bootloader. Kconfig options can be used to alter
 * the behavior of this builtin command line.
 * @dest: The destination of the final appended/prepended string
 * @tmp: temporary space used for prepending
 * @prepend: string to prepend to @dest
 * @append: string to append to @dest
 * @length: the maximum length of the strings above.
 * @cmdline_strlen: point to a compatible strlen
 * @cmdline_strlcat: point to a compatible strlcat
 * This function returns true when the builtin command line was copied successfully
 * and false when there was not enough room to copy all parts of the command line.
 */
static inline bool
__cmdline_add_builtin(
		char *dest,
		char *tmp,
		char *prepend,
		char *append,
		unsigned long length,
		size_t (*cmdline_strlen)(const char *s),
		size_t (*cmdline_strlcat)(char *dest, const char *src, size_t count))
{
	size_t total_length = 0, tmp_length;

	if (!IS_ENABLED(CONFIG_GENERIC_CMDLINE))
		return true;

	if (!IS_ENABLED(CONFIG_CMDLINE_BOOL))
		return true;

	if (IS_ENABLED(CONFIG_CMDLINE_OVERRIDE))
		dest[0] = '\0';
	else
		total_length += cmdline_strlen(dest);

	tmp_length = cmdline_strlen(append);
	if (tmp_length > 0) {
		cmdline_strlcat(dest, append, length);
		total_length += tmp_length;
	}

	tmp_length = cmdline_strlen(prepend);
	if (tmp_length > 0) {
		cmdline_strlcat(tmp, prepend, length);
		cmdline_strlcat(tmp, dest, length);
		dest[0] = '\0';
		cmdline_strlcat(dest, tmp, length);
		total_length += tmp_length;
	}

	tmp[0] = '\0';

	if (total_length > length)
		return false;

	return true;
}

#define cmdline_add_builtin(dest) \
	__cmdline_add_builtin(dest, CMDLINE_TMP, CMDLINE_PREPEND, CMDLINE_APPEND, COMMAND_LINE_SIZE, CMDLINE_STRLEN, CMDLINE_STRLCAT)

#define cmdline_get_static_builtin(dest) \
	(CMDLINE_STATIC_PREPEND CMDLINE_STATIC_APPEND)

#ifndef CONFIG_GENERIC_CMDLINE
static inline bool of_deprecated_cmdline_update(char *cmdline, const char *dt_bootargs, int length)
{
	if (dt_bootargs != NULL && length > 0)
		strlcpy(cmdline, dt_bootargs, min(length, COMMAND_LINE_SIZE));
	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 */

#ifdef CONFIG_CMDLINE
#if defined(CONFIG_CMDLINE_EXTEND)
	strlcat(cmdline, " ", COMMAND_LINE_SIZE);
	strlcat(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#elif defined(CONFIG_CMDLINE_FORCE)
	strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#else
	/* No arguments from boot loader, use kernel's  cmdl*/
	if (!((char *)cmdline)[0])
		strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#endif
#endif /* CONFIG_CMDLINE */
	return true;
}
#else
static inline bool of_deprecated_cmdline_update(char *cmdline, const char *dt_bootargs, int length) { return false; }
#endif /* CONFIG_GENERIC_CMDLINE */


#endif /* _LINUX_CMDLINE_H */
