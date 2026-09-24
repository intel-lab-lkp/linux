/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ORC_H
#define _LINUX_ORC_H

#include <asm/orc_types.h>

static inline unsigned long orc_ip(const int *ip)
{
	return (unsigned long)ip + *ip;
}

struct orc_entry *__orc_find(int *ip_table, struct orc_entry *u_table,
			     unsigned int num_entries, unsigned long ip);

#ifdef CONFIG_MODULES
struct orc_entry *orc_module_find(unsigned long ip);
#else
static inline struct orc_entry *orc_module_find(unsigned long ip)
{
	return NULL;
}
#endif

#endif /* _LINUX_ORC_H */
