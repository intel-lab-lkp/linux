/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KSTACKWATCH_H
#define _KSTACKWATCH_H

#include <linux/types.h>

#define MAX_FUNC_NAME_LEN 64
#define MAX_CONFIG_STR_LEN 128
#define MAX_FRAME_SEARCH 128

enum watch_type {
	WATCH_CANARY = 0,
	WATCH_LOCAL_VAR,
};

struct ksw_config {
	/* function part */
	char function[MAX_FUNC_NAME_LEN];
	u16 ip_offset;
	u16 depth;

	/* local var, useless for canary watch */
	/* offset from rsp at function+ip_offset */
	u16 local_var_offset;

	/*
	 * local var size (1,2,4,8 bytes)
	 * it will be the watching len
	 */
	u16 local_var_len;

	/* easy for understand*/
	enum watch_type type;

	/* save to show */
	char config_str[MAX_CONFIG_STR_LEN];
};

#endif /* _KSTACKWATCH_H */
