/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __LINUX_P4TC_H
#define __LINUX_P4TC_H

#define P4TC_MAX_KEYSZ 512

enum {
	P4T_UNSPEC,
	P4T_U8,
	P4T_U16,
	P4T_U32,
	P4T_U64,
	P4T_STRING,
	P4T_S8,
	P4T_S16,
	P4T_S32,
	P4T_S64,
	P4T_MACADDR,
	P4T_IPV4ADDR,
	P4T_BE16,
	P4T_BE32,
	P4T_BE64,
	P4T_U128,
	P4T_S128,
	P4T_BOOL,
	P4T_DEV,
	P4T_KEY,
	__P4T_MAX,
};

#define P4T_MAX (__P4T_MAX - 1)

#endif
