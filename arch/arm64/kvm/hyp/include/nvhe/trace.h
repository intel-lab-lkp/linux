/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM64_KVM_HYP_NVHE_TRACE_H
#define __ARM64_KVM_HYP_NVHE_TRACE_H

#include <linux/trace_remote_event.h>

#include <asm/kvm_hyptrace.h>

#define HE_PROTO(__args...)	__args

#ifdef CONFIG_PKVM_TRACING
void *tracing_reserve_entry(unsigned long length);
void tracing_commit_entry(void);

#define HE_ASSIGN(__args...)	__args
#define HE_STRUCT		RE_STRUCT
#define he_field		re_field

#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)		\
	REMOTE_EVENT_FORMAT(__name, __struct);					\
	extern atomic_t __name##_enabled;					\
	extern struct hyp_event_id hyp_event_id_##__name;			\
	static __always_inline void trace_##__name(__proto)			\
	{									\
		struct remote_event_format_##__name *__entry;			\
		size_t length = sizeof(*__entry);				\
										\
		if (!atomic_read(&__name##_enabled))				\
			return;							\
		__entry = tracing_reserve_entry(length);			\
		if (!__entry)							\
			return;							\
		__entry->hdr.id = hyp_event_id_##__name.id;			\
		__assign							\
		tracing_commit_entry();						\
	}

void __pkvm_update_clock_tracing(u32 mult, u32 shift, u64 epoch_ns, u64 epoch_cyc);
int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size);
void __pkvm_unload_tracing(void);
int __pkvm_enable_tracing(bool enable);
int __pkvm_reset_tracing(unsigned int cpu);
int __pkvm_swap_reader_tracing(unsigned int cpu);
int __pkvm_enable_event(unsigned short id, bool enable);
#else
static inline void *tracing_reserve_entry(unsigned long length) { return NULL; }
static inline void tracing_commit_entry(void) { }
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)      \
	static inline void trace_##__name(__proto) {}

static inline
void __pkvm_update_clock_tracing(u32 mult, u32 shift, u64 epoch_ns, u64 epoch_cyc) { }
static inline int __pkvm_load_tracing(unsigned long desc_va, size_t desc_size) { return -ENODEV; }
static inline void __pkvm_unload_tracing(void) { }
static inline int __pkvm_enable_tracing(bool enable) { return -ENODEV; }
static inline int __pkvm_reset_tracing(unsigned int cpu) { return -ENODEV; }
static inline int __pkvm_swap_reader_tracing(unsigned int cpu) { return -ENODEV; }
static inline int __pkvm_enable_event(unsigned short id, bool enable)  { return -ENODEV; }
#endif
#endif
