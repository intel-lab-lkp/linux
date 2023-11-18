/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mce

#if !defined(_TRACE_MCE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MCE_H

#include <linux/ktime.h>
#include <linux/tracepoint.h>
#include <asm/mce.h>

TRACE_EVENT(mce_record,

	TP_PROTO(struct mce_hw_err *err),

	TP_ARGS(err),

	TP_STRUCT__entry(
		__field(	u64,		mcgcap		)
		__field(	u64,		mcgstatus	)
		__field(	u64,		status		)
		__field(	u64,		addr		)
		__field(	u64,		misc		)
		__field(	u64,		synd		)
		__field(	u64,		ipid		)
		__field(	u64,		ip		)
		__field(	u64,		tsc		)
		__field(	u64,		walltime	)
		__field(	u32,		cpu		)
		__field(	u32,		cpuid		)
		__field(	u32,		apicid		)
		__field(	u32,		socketid	)
		__field(	u8,		cs		)
		__field(	u8,		bank		)
		__field(	u8,		cpuvendor	)
		__field(	u8,     len	)
		__dynamic_array(u8, v_data, sizeof(err->vi))
	),

	TP_fast_assign(
		__entry->mcgcap		= err->m.mcgcap;
		__entry->mcgstatus	= err->m.mcgstatus;
		__entry->status		= err->m.status;
		__entry->addr		= err->m.addr;
		__entry->misc		= err->m.misc;
		__entry->synd		= err->m.synd;
		__entry->ipid		= err->m.ipid;
		__entry->ip		= err->m.ip;
		__entry->tsc		= err->m.tsc;
		__entry->walltime	= err->m.time;
		__entry->cpu		= err->m.extcpu;
		__entry->cpuid		= err->m.cpuid;
		__entry->apicid		= err->m.apicid;
		__entry->socketid	= err->m.socketid;
		__entry->cs		= err->m.cs;
		__entry->bank		= err->m.bank;
		__entry->cpuvendor	= err->m.cpuvendor;
		__entry->len	= sizeof(err->vi);
		memcpy(__get_dynamic_array(v_data), &err->vi, sizeof(err->vi));
	),

	TP_printk("CPU: %d, MCGc/s: %llx/%llx, MC%d: %016llx, IPID: %016llx, ADDR/MISC/SYND: %016llx/%016llx/%016llx, RIP: %02x:<%016llx>, TSC: %llx, PROCESSOR: %u:%x, TIME: %llu, SOCKET: %u, APIC: %x, Vendor Data: %s",
		__entry->cpu,
		__entry->mcgcap, __entry->mcgstatus,
		__entry->bank, __entry->status,
		__entry->ipid,
		__entry->addr, __entry->misc, __entry->synd,
		__entry->cs, __entry->ip,
		__entry->tsc,
		__entry->cpuvendor, __entry->cpuid,
		__entry->walltime,
		__entry->socketid,
		__entry->apicid,
		__print_array(__get_dynamic_array(v_data), __entry->len / 8, 8))
);

#endif /* _TRACE_MCE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
