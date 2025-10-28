/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TOOLS_LINUX_KBUILD_H
#define __TOOLS_LINUX_KBUILD_H

#include <stddef.h>

#define DEFINE(sym, val) \
	asm volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i" (val))

#define BLANK() asm volatile("\n.ascii \"->\"" : : )

#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem))

#define COMMENT(x) \
	asm volatile("\n.ascii \"->#" x "\"")

#endif /* __TOOLS_LINUX_KBUILD_H */
