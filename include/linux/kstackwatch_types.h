/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSTACKWATCH_TYPES_H
#define _LINUX_KSTACKWATCH_TYPES_H
#include <linux/types.h>

struct ksw_watchpoint;
struct ksw_ctx {
	struct ksw_watchpoint *wp;
	ulong sp;
	u16 depth;
	u16 generation;
};

#endif /* _LINUX_KSTACKWATCH_TYPES_H */
