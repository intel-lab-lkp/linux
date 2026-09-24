/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM riscv_tlb

#if !defined(_TRACE_RISCV_TLB_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RISCV_TLB_H

#include <linux/cpumask.h>
#include <linux/tracepoint.h>

#ifndef _TRACE_RISCV_TLB_ENUMS
#define _TRACE_RISCV_TLB_ENUMS

enum riscv_tlb_flush_scope {
	RISCV_TLB_FLUSH_SCOPE_SINGLE,
	RISCV_TLB_FLUSH_SCOPE_RANGE,
	RISCV_TLB_FLUSH_SCOPE_ADDRESS_SPACE,
	RISCV_TLB_FLUSH_SCOPE_ALL,
};

enum riscv_tlb_flush_path {
	RISCV_TLB_FLUSH_PATH_LOCAL,
	RISCV_TLB_FLUSH_PATH_SBI_RFENCE,
	RISCV_TLB_FLUSH_PATH_CROSS_CPU_CALL,
};

#endif /* _TRACE_RISCV_TLB_ENUMS */

TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_SCOPE_SINGLE);
TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_SCOPE_RANGE);
TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_SCOPE_ADDRESS_SPACE);
TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_SCOPE_ALL);

TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_PATH_LOCAL);
TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_PATH_SBI_RFENCE);
TRACE_DEFINE_ENUM(RISCV_TLB_FLUSH_PATH_CROSS_CPU_CALL);

#define show_riscv_tlb_flush_scope(scope) \
	__print_symbolic(scope, \
		{ RISCV_TLB_FLUSH_SCOPE_SINGLE,        "single" }, \
		{ RISCV_TLB_FLUSH_SCOPE_RANGE,         "range" }, \
		{ RISCV_TLB_FLUSH_SCOPE_ADDRESS_SPACE, "address-space" }, \
		{ RISCV_TLB_FLUSH_SCOPE_ALL,           "all" })

#define show_riscv_tlb_flush_path(path) \
	__print_symbolic(path, \
		{ RISCV_TLB_FLUSH_PATH_LOCAL,          "local" }, \
		{ RISCV_TLB_FLUSH_PATH_SBI_RFENCE,     "sbi-rfence" }, \
		{ RISCV_TLB_FLUSH_PATH_CROSS_CPU_CALL, "cross-cpu-call" })

/*
 * Record the invalidation request received by the RISC-V architecture code
 * and the path selected by Linux.
 *
 * The target CPU mask represents the CPUs Linux intends to cover for the
 * request. It can be correlated with per-CPU activity, but does not describe
 * which harts ultimately performed an invalidation.
 *
 * The ASID is hardware-visible and may be reused. It must not be treated as a
 * persistent identifier for an mm.
 *
 * The stride describes the invalidation granularity supplied to the RISC-V
 * implementation. SBI RFENCE receives start, size and ASID, but not stride.
 *
 * The event is emitted at path selection time. For SBI RFENCE, it records
 * delegation of the request to firmware; firmware processing after that
 * point is outside the event's scope.
 */
TRACE_EVENT(riscv_tlb_flush_path,
	TP_PROTO(unsigned long start, unsigned long size,
		 unsigned long stride, unsigned long asid, bool has_mm,
		 const struct cpumask *cmask,
		 enum riscv_tlb_flush_scope scope,
		 enum riscv_tlb_flush_path path),

	TP_ARGS(start, size, stride, asid, has_mm, cmask, scope, path),

	TP_STRUCT__entry(
		__field(unsigned long, start)
		__field(unsigned long, size)
		__field(unsigned long, stride)
		__field(unsigned long, asid)
		__field(bool, has_mm)
		__field(unsigned int, target_mask_weight)
		__cpumask(target_cpus)
		__field(u8, scope)
		__field(u8, path)
	),

	TP_fast_assign(
		__entry->start = start;
		__entry->size = size;
		__entry->stride = stride;
		__entry->asid = asid;
		__entry->has_mm = has_mm;
		__entry->target_mask_weight = cpumask_weight(cmask);
		__assign_cpumask(target_cpus, cpumask_bits(cmask));
		__entry->scope = scope;
		__entry->path = path;
	),

	TP_printk("start=%#lx size=%#lx stride=%#lx asid=%#lx has_mm=%d target_mask_weight=%u target_cpus=%s scope=%s path=%s",
		  __entry->start, __entry->size, __entry->stride,
		  __entry->asid, __entry->has_mm,
		  __entry->target_mask_weight, __get_cpumask(target_cpus),
		  show_riscv_tlb_flush_scope(__entry->scope),
		  show_riscv_tlb_flush_path(__entry->path))
);

#endif /* _TRACE_RISCV_TLB_H */

/* This part must be outside protection. */
#include <trace/define_trace.h>
