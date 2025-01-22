/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Samsung */
#ifndef __MODERR_H
#define __MODERR_H

#define MAX_PARAM_PREFIX_LEN (64 - sizeof(unsigned long))
#define MODULE_NAME_LEN MAX_PARAM_PREFIX_LEN
#define MODULE_FUNC_LEN 128
#define MESSAGE_LEN 128

enum modfunc {
	UNKNOWN,
	COMPLETE_FORMATION = 1,
	DO_INIT_MODULE,
	MODULE_ENABLE_RODATA_AFTER_INIT,
};

struct event {
	char modname[MODULE_NAME_LEN];
	int err;
	int func;
	char msg[MESSAGE_LEN];
	int dbg;
};

static inline const char *modfunc_to_string(enum modfunc fc)
{
	switch (fc) {
	case COMPLETE_FORMATION:
		return "complete_formation()";
	case DO_INIT_MODULE:
		return "do_init_module()";
	case MODULE_ENABLE_RODATA_AFTER_INIT:
		return "module_enable_rodata_after_init()";
	default:
		return "unknown";
	}
}

#endif /* __MODERR_H */
