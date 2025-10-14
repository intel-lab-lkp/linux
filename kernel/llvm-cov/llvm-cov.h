/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019	Sami Tolvanen <samitolvanen@google.com>, Google, Inc.
 * Copyright (C) 2024	Jinghao Jia   <jinghao7@illinois.edu>,   UIUC
 * Copyright (C) 2024	Wentao Zhang  <wentaoz5@illinois.edu>,   UIUC
 */

#ifndef _LLVM_COV_H
#define _LLVM_COV_H

extern spinlock_t llvm_cov_lock;

static __always_inline unsigned long llvm_cov_claim_lock(void)
{
	unsigned long flags;

	spin_lock_irqsave(&llvm_cov_lock, flags);

	return flags;
}

static __always_inline void llvm_cov_release_lock(unsigned long flags)
{
	spin_unlock_irqrestore(&llvm_cov_lock, flags);
}

/*
 * Note: These internal LLVM definitions must match the compiler version.
 * See llvm/include/llvm/ProfileData/InstrProfData.inc in LLVM's source code.
 */

#define INSTR_PROF_RAW_MAGIC_64		\
		((u64)255 << 56 |	\
		 (u64)'l' << 48 |	\
		 (u64)'p' << 40 |	\
		 (u64)'r' << 32 |	\
		 (u64)'o' << 24 |	\
		 (u64)'f' << 16 |	\
		 (u64)'r' << 8  |	\
		 (u64)129)

#if defined(CONFIG_CC_IS_CLANG) && CONFIG_CLANG_VERSION >= 190000
/*
 * LLVM 19 introduced profile format version 10 (commit llvm/llvm-project@c30b7e0)
 * which added vtable profiling support (IPVK_VTableTarget).
 * If not updated, __llvm_profile_header size mismatch will cause profile
 * generation to fail silently, resulting in an empty or corrupted profraw file.
 */
#define INSTR_PROF_RAW_VERSION		10
#define INSTR_PROF_DATA_ALIGNMENT	8
#define IPVK_LAST			2
#elif defined(CONFIG_CC_IS_CLANG) && CONFIG_CLANG_VERSION >= 180000
/*
 * LLVM 18 uses profile format version 9 with temporal profiling support.
 * See llvm/include/llvm/ProfileData/InstrProfData.inc for the authoritative
 * source of these version mappings.
 */
#define INSTR_PROF_RAW_VERSION		9
#define INSTR_PROF_DATA_ALIGNMENT	8
#define IPVK_LAST			1
#endif

/**
 * struct __llvm_profile_header - represents the raw profile header data
 * structure. Description of each member can be found here:
 * https://llvm.org/docs/InstrProfileFormat.html#header.
 */
struct __llvm_profile_header {
	u64 magic;
	u64 version;
	u64 binary_ids_size;
	u64 num_data;
	u64 padding_bytes_before_counters;
	u64 num_counters;
	u64 padding_bytes_after_counters;
	u64 num_bitmap_bytes;
	u64 padding_bytes_after_bitmap_bytes;
	u64 names_size;
	u64 counters_delta;
	u64 bitmap_delta;
	u64 names_delta;
#if defined(CONFIG_CC_IS_CLANG) && CONFIG_CLANG_VERSION >= 190000
	u64 num_v_tables;
	u64 v_names_size;
#endif
	u64 value_kind_last;
};

/**
 * struct __llvm_profile_data - represents the per-function control structure.
 * Description of each member can be found here:
 * https://llvm.org/docs/InstrProfileFormat.html#profile-metadata. To measure
 * Source-based Code Coverage, the internals of this struct don't matter at run
 * time. The only purpose of the definition below is to run sizeof() against it
 * so that we can calculate the "num_data" field in header.
 */
struct __llvm_profile_data {
	const u64 name_ref;
	const u64 func_hash;
	const void *counter_ptr;
	const void *bitmap_ptr;
	const void *function_pointer;
	void *values;
	const u32 num_counters;
	const u16 num_value_sites[IPVK_LAST + 1];
	const u32 num_bitmap_bytes;
} __aligned(INSTR_PROF_DATA_ALIGNMENT);

/* Payload sections */

extern struct __llvm_profile_data __llvm_prf_data_start[];
extern struct __llvm_profile_data __llvm_prf_data_end[];

extern u64 __llvm_prf_cnts_start[];
extern u64 __llvm_prf_cnts_end[];

extern char __llvm_prf_names_start[];
extern char __llvm_prf_names_end[];

extern char __llvm_prf_bits_start[];
extern char __llvm_prf_bits_end[];

#define __DEFINE_SECTION_SIZE(s)					\
	static inline unsigned long __llvm_prf_ ## s ## _size(void)	\
	{								\
		unsigned long start =					\
			(unsigned long)__llvm_prf_ ## s ## _start;	\
		unsigned long end =					\
			(unsigned long)__llvm_prf_ ## s ## _end;	\
		return end - start;					\
	}
#define __DEFINE_SECTION_COUNT(s)					\
	static inline unsigned long __llvm_prf_ ## s ## _count(void)	\
	{								\
		return __llvm_prf_ ## s ## _size() /			\
			sizeof(__llvm_prf_ ## s ## _start[0]);		\
	}

__DEFINE_SECTION_SIZE(data)
__DEFINE_SECTION_SIZE(cnts)
__DEFINE_SECTION_SIZE(names)
__DEFINE_SECTION_SIZE(bits)

__DEFINE_SECTION_COUNT(data)
__DEFINE_SECTION_COUNT(cnts)
__DEFINE_SECTION_COUNT(names)
__DEFINE_SECTION_COUNT(bits)

#undef __DEFINE_SECTION_SIZE
#undef __DEFINE_SECTION_COUNT

static inline unsigned long __llvm_prf_get_padding(unsigned long size)
{
	return 7 & (sizeof(u64) - size % sizeof(u64));
}

#endif /* _LLVM_COV_H */
