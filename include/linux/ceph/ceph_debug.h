/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_DEBUG_H
#define _FS_CEPH_DEBUG_H

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/string.h>

#ifdef CONFIG_CEPH_LIB_PRETTYDEBUG

/*
 * Pretty debug output metadata: Enhanced debugging infrastructure that provides
 * detailed context information including filenames, line numbers, and client
 * identification. Incurs additional overhead but significantly improves debugging
 * capabilities for complex distributed system interactions.
 */

/*
 * Active debug macros: Full-featured debugging with file/line context.
 * Format: "MODULE FILE:LINE : message" with optional client identification.
 */
# if defined(DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
#  define dout(fmt, ...)						\
	pr_debug("%.*s %12.12s:%-4d : " fmt,				\
		 8 - (int)sizeof(KBUILD_MODNAME), "    ",		\
		 kbasename(__FILE__), __LINE__, ##__VA_ARGS__)
#  define doutc(client, fmt, ...)					\
	pr_debug("%.*s %12.12s:%-4d : [%pU %llu] " fmt,			\
		 8 - (int)sizeof(KBUILD_MODNAME), "    ",		\
		 kbasename(__FILE__), __LINE__,				\
		 &client->fsid, client->monc.auth->global_id,		\
		 ##__VA_ARGS__)
# else
/*
 * Compile-time debug validation: No-op macros that preserve format string
 * checking without generating debug output. Catches format errors at compile time.
 */
#  define dout(fmt, ...)					\
		no_printk(KERN_DEBUG fmt, ##__VA_ARGS__)
#  define doutc(client, fmt, ...)				\
		no_printk(KERN_DEBUG "[%pU %llu] " fmt,		\
			  &client->fsid,			\
			  client->monc.auth->global_id,		\
			  ##__VA_ARGS__)
# endif

#else

/*
 * Simple debug output metadata: Basic debugging without filename/line context.
 * Lighter weight alternative that includes client identification and function names.
 */
# define dout(fmt, ...)	pr_debug(" " fmt, ##__VA_ARGS__)
# define doutc(client, fmt, ...)					\
	pr_debug(" [%pU %llu] %s: " fmt, &client->fsid,			\
		 client->monc.auth->global_id, __func__, ##__VA_ARGS__)

#endif

/*
 * Client-aware logging macros: Production logging infrastructure that includes
 * client identification (FSID + global ID) in all messages. Essential for
 * debugging multi-client scenarios and cluster-wide issues.
 * Format: "[FSID GLOBAL_ID]: message"
 */
#define pr_notice_client(client, fmt, ...)				\
	pr_notice("[%pU %llu]: " fmt, &client->fsid,			\
		  client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_info_client(client, fmt, ...)				\
	pr_info("[%pU %llu]: " fmt, &client->fsid,			\
		client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_warn_client(client, fmt, ...)				\
	pr_warn("[%pU %llu]: " fmt, &client->fsid,			\
		client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_warn_once_client(client, fmt, ...)				\
	pr_warn_once("[%pU %llu]: " fmt, &client->fsid,			\
		     client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_err_client(client, fmt, ...)					\
	pr_err("[%pU %llu]: " fmt, &client->fsid,			\
	       client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_warn_ratelimited_client(client, fmt, ...)			\
	pr_warn_ratelimited("[%pU %llu]: " fmt, &client->fsid,		\
			    client->monc.auth->global_id, ##__VA_ARGS__)
#define pr_err_ratelimited_client(client, fmt, ...)			\
	pr_err_ratelimited("[%pU %llu]: " fmt, &client->fsid,		\
			   client->monc.auth->global_id, ##__VA_ARGS__)

#endif
