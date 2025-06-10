/* SPDX-License-Identifier: GPL-2.0 */

#if !defined(_TRACE_BONDING_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_BONDING_H

#include <linux/netdevice.h>
#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM bonding

TRACE_EVENT(3ad_mux_state,
	TP_PROTO(struct net_device *dev, u32 last_state, u32 curr_state),
	TP_ARGS(dev, last_state, curr_state),

	TP_STRUCT__entry(
		__field(int, ifindex)
		__string(dev_name, dev->name)
		__field(u32, last_state)
		__field(u32, curr_state)
	),

	TP_fast_assign(
		__entry->ifindex = dev->ifindex;
		__assign_str(dev_name);
		__entry->last_state = last_state;
		__entry->curr_state = curr_state;
	),

	TP_printk("ifindex %d dev %s last_state 0x%x curr_state 0x%x",
		  __entry->ifindex, __get_str(dev_name),
		  __entry->last_state, __entry->curr_state)
);

#endif /* _TRACE_BONDING_H */

#include <trace/define_trace.h>
