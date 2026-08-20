/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TLOB_KUNIT_H
#define __TLOB_KUNIT_H

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)

#include <linux/types.h>

extern const struct rv_tlob_kunit_ops {
	int (*parse_uprobe_line)(char *buf, u64 *thr_out, char **path_out,
				 loff_t *start_out, loff_t *stop_out);
	int (*parse_remove_line)(char *buf, char **path_out, loff_t *start_out);
} rv_tlob_kunit_ops;

#endif

#endif /* __TLOB_KUNIT_H */
