/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KSTACKWATCH_H
#define _KSTACKWATCH_H

#include <linux/types.h>

#define MAX_CONFIG_STR_LEN 128

struct ksw_config {
	char *func_name;
	u16 depth;

	/*
	 * watched variable info:
	 * - func_offset : instruction offset in the function, typically the
	 *                 assignment of the watched variable, where ksw
	 *                 registers a kprobe post-handler.
	 * - sp_offset   : offset from stack pointer at func_offset. Usually 0.
	 * - watch_len   : size of the watched variable (1, 2, 4, or 8 bytes).
	 */
	u16 func_offset;
	u16 sp_offset;
	u16 watch_len;

	/* max number of hwbps that can be used */
	u16 max_watch;

	/* search canary as watch target automatically */
	u16 auto_canary;

	/* panic on watchpoint hit */
	u16 panic_hit;

	/* save to show */
	char *user_input;
};

#endif /* _KSTACKWATCH_H */
