/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUNC_META_H
#define _LINUX_FUNC_META_H

#include <linux/kernel.h>

struct func_meta {
	int users;
	void *func;
};

extern struct func_meta *func_metas;

struct func_meta *func_meta_get(void *ip);
void func_meta_put(void *ip, struct func_meta *meta);

#endif
