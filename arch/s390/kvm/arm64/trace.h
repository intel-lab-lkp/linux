/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(KVM_ARM64_TRACE_KVM_H) || defined(TRACE_HEADER_MULTI_READ)
#define KVM_ARM64_TRACE_KVM_H

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM kvm_arm64

TRACE_EVENT(kvm_mmio_nisv,
	TP_PROTO(unsigned long vcpu_pc, unsigned long esr,
		 unsigned long far, unsigned long ipa),
	TP_ARGS(vcpu_pc, esr, far, ipa),

	TP_STRUCT__entry(
		__field(	unsigned long,	vcpu_pc		)
		__field(	unsigned long,	esr		)
		__field(	unsigned long,	far		)
		__field(	unsigned long,	ipa		)
	),

	TP_fast_assign(
		__entry->vcpu_pc		= vcpu_pc;
		__entry->esr			= esr;
		__entry->far			= far;
		__entry->ipa			= ipa;
	),

	TP_printk("ipa %#016lx, esr %#016lx, far %#016lx, pc %#016lx",
		  __entry->ipa, __entry->esr,
		  __entry->far, __entry->vcpu_pc)
);

#endif /* KVM_ARM64_TRACE_KVM_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

/* This part must be outside protection */
#include <trace/define_trace.h>
