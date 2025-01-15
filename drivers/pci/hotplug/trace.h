/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_TRACE_HW_EVENT_PCI_HP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HW_EVENT_PCI_HP_H

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM pci

TRACE_EVENT(pci_hp_event,

	TP_PROTO(const char *port_name,
		 const char *slot,
		 const int event),

	TP_ARGS(port_name, slot, event),

	TP_STRUCT__entry(
		__string(	port_name,	port_name	)
		__string(	slot,		slot		)
		__field(	int,		event	)
	),

	TP_fast_assign(
		__assign_str(port_name);
		__assign_str(slot);
		__entry->event = event;
	),

	TP_printk("%s slot:%s, event:%s\n",
		__get_str(port_name),
		__get_str(slot),
		__print_symbolic(__entry->event, PCI_HOTPLUG_EVENT)
	)
);

#endif /* _TRACE_HW_EVENT_PCI_HP_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH  ../../drivers/pci/hotplug
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

/* This part must be outside protection */
#include <trace/define_trace.h>
