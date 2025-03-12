/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _NET_CORE_LWTUNNEL_H
#define _NET_CORE_LWTUNNEL_H

#include <linux/netdevice.h>

#define LWTUNNEL_RECURSION_LIMIT 8

#ifndef CONFIG_PREEMPT_RT
static inline bool lwtunnel_recursion(void)
{
	return unlikely(__this_cpu_read(softnet_data.xmit.recursion) >
			LWTUNNEL_RECURSION_LIMIT);
}

static inline void lwtunnel_recursion_inc(void)
{
	__this_cpu_inc(softnet_data.xmit.recursion);
}

static inline void lwtunnel_recursion_dec(void)
{
	__this_cpu_dec(softnet_data.xmit.recursion);
}
#else
static inline bool lwtunnel_recursion(void)
{
	return unlikely(current->net_xmit.recursion > LWTUNNEL_RECURSION_LIMIT);
}

static inline void lwtunnel_recursion_inc(void)
{
	current->net_xmit.recursion++;
}

static inline void lwtunnel_recursion_dec(void)
{
	current->net_xmit.recursion--;
}
#endif

#endif /* _NET_CORE_LWTUNNEL_H */
