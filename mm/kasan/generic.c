// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains core generic KASAN code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */

#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/kfence.h>
#include <linux/kmemleak.h>
#include <linux/linkage.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/bug.h>

#include "kasan.h"
#include "../slab.h"

/*
 * All functions below always inlined so compiler could
 * perform better optimizations in each of __asan_loadX/__assn_storeX
 * depending on memory access size X.
 */

#ifdef CONFIG_KASAN_MEM_TRACK
#define KASAN_SHADOW_VALUE_MASK_ONE_BYTE	0x07
#define KASAN_TRACK_VALUE_MASK_ONE_BYTE		0x78
#define KASAN_SHADOW_VALUE_MASK_TWO_BYTE	0x0707
#define KASAN_SHADOW_VALUE_MASK_EIGHT_BYTE	0x0707070707070707
#define KASAN_TRACK_VALUE_MASK_EIGHT_BYTE	0x7878787878787878
#define KASAN_TRACK_VALUE_OFFSET			3
static __always_inline bool is_poison_value_1_byte(s8 shadow_value)
{
	if (shadow_value & 0x80)
		return true;
	return false;
}

static __always_inline bool is_poison_value_8_byte(u64 shadow_value)
{
	if (shadow_value & 0x8080808080808080)
		return true;
	return false;
}

static __always_inline s8 to_shadow_value_1_byte(s8 shadow_value)
{
	if (is_poison_value_1_byte(shadow_value))
		return shadow_value;
	return shadow_value & KASAN_SHADOW_VALUE_MASK_ONE_BYTE;
}

static __always_inline s8 to_track_value_1_byte(s8 shadow_value)
{
	if (is_poison_value_1_byte(shadow_value))
		return shadow_value;
	return (shadow_value & KASAN_TRACK_VALUE_MASK_ONE_BYTE) >>
				KASAN_TRACK_VALUE_OFFSET;
}

static __always_inline u64 to_shadow_value_8_byte(u64 shadow_value)
{
	if (is_poison_value_8_byte(shadow_value))
		return shadow_value;
	return shadow_value & KASAN_SHADOW_VALUE_MASK_EIGHT_BYTE;
}

static __always_inline u64 to_track_value_8_byte(u64 shadow_value)
{
	if (is_poison_value_8_byte(shadow_value))
		return shadow_value;
	return shadow_value & KASAN_TRACK_VALUE_MASK_EIGHT_BYTE;
}

static __always_inline s8 get_shadow_value_1_byte(const void *addr)
{
	s8 shadow_value = *(s8 *)kasan_mem_to_shadow(addr);
	return to_shadow_value_1_byte(shadow_value);
}

static __always_inline u16 get_shadow_value_2_byte(const void *addr)
{
	u16 shadow_value = *(u16 *)kasan_mem_to_shadow(addr);

	return shadow_value & KASAN_SHADOW_VALUE_MASK_TWO_BYTE;
}
#else
static __always_inline s8 to_shadow_value_1_byte(s8 shadow_value)
{
	return shadow_value;
}
static __always_inline u64 to_shadow_value_8_byte(u64 shadow_value)
{
	return shadow_value;
}
static __always_inline s8 get_shadow_value_1_byte(const void *addr)
{
	return *(s8 *)kasan_mem_to_shadow(addr);
}
static __always_inline u16 get_shadow_value_2_byte(const void *addr)
{
	return *(u16 *)kasan_mem_to_shadow(addr);
}
static __always_inline bool memory_is_tracked(const void *addr, size_t size)
{
	return 0;
}
#endif

static __always_inline bool memory_is_poisoned_1(const void *addr)
{
	s8 shadow_value = get_shadow_value_1_byte(addr);

	if (unlikely(shadow_value)) {
		s8 last_accessible_byte = (unsigned long)addr & KASAN_GRANULE_MASK;
		return unlikely(last_accessible_byte >= shadow_value);
	}

	return false;
}

static __always_inline bool memory_is_poisoned_2_4_8(const void *addr,
						unsigned long size)
{
	/*
	 * Access crosses 8(shadow size)-byte boundary. Such access maps
	 * into 2 shadow bytes, so we need to check them both.
	 */
	if (unlikely((((unsigned long)addr + size - 1) & KASAN_GRANULE_MASK) < size - 1))
		return get_shadow_value_1_byte(addr) || memory_is_poisoned_1(addr + size - 1);

	return memory_is_poisoned_1(addr + size - 1);
}

static __always_inline bool memory_is_poisoned_16(const void *addr)
{
	/* Unaligned 16-bytes access maps into 3 shadow bytes. */
	if (unlikely(!IS_ALIGNED((unsigned long)addr, KASAN_GRANULE_SIZE)))
		return get_shadow_value_2_byte(addr) || memory_is_poisoned_1(addr + 15);

	return get_shadow_value_2_byte(addr);
}

static __always_inline unsigned long bytes_is_nonzero(const s8 *start,
					size_t size)
{
	while (size) {
		if (unlikely(to_shadow_value_1_byte(*start)))
			return (unsigned long)start;
		start++;
		size--;
	}

	return 0;
}

static __always_inline unsigned long shadow_val_is_nonzero(const void *start,
						const void *end)
{
	unsigned int words;
	unsigned long ret;
	unsigned int prefix = (unsigned long)start % 8;

	if (end - start <= 16)
		return bytes_is_nonzero(start, end - start);

	if (prefix) {
		prefix = 8 - prefix;
		ret = bytes_is_nonzero(start, prefix);
		if (unlikely(ret))
			return ret;
		start += prefix;
	}

	words = (end - start) / 8;
	while (words) {
		if (unlikely(to_shadow_value_8_byte(*(u64 *)start)))
			return bytes_is_nonzero(start, 8);
		start += 8;
		words--;
	}

	return bytes_is_nonzero(start, (end - start) % 8);
}

static __always_inline bool memory_is_poisoned_n(const void *addr, size_t size)
{
	unsigned long ret;

	ret = shadow_val_is_nonzero(kasan_mem_to_shadow(addr),
			kasan_mem_to_shadow(addr + size - 1) + 1);

	if (unlikely(ret)) {
		const void *last_byte = addr + size - 1;
		s8 *last_shadow = (s8 *)kasan_mem_to_shadow(last_byte);
		s8 last_accessible_byte = (unsigned long)last_byte & KASAN_GRANULE_MASK;

		if (unlikely(ret != (unsigned long)last_shadow ||
			     last_accessible_byte >= to_shadow_value_1_byte(*last_shadow)))
			return true;
	}
	return false;
}

static __always_inline bool memory_is_poisoned(const void *addr, size_t size)
{
	if (__builtin_constant_p(size)) {
		switch (size) {
		case 1:
			return memory_is_poisoned_1(addr);
		case 2:
		case 4:
		case 8:
			return memory_is_poisoned_2_4_8(addr, size);
		case 16:
			return memory_is_poisoned_16(addr);
		default:
			BUILD_BUG();
		}
	}

	return memory_is_poisoned_n(addr, size);
}

#ifdef CONFIG_KASAN_MEM_TRACK
static __always_inline s8 get_track_value(const void *addr)
{
	s8 shadow_value = *(s8 *)kasan_mem_to_shadow(addr);

	/* In the early stages of system startup, when Kasan is not fully ready,
	 * some illegal values may be obtained. Ignore it.
	 */
	if (unlikely(shadow_value & 0x80))
		return 0;
	return (shadow_value >> KASAN_TRACK_VALUE_OFFSET);
}

/* ================================== size :	  1     2     3     4     5     6     7    8 */
static const s8 kasan_track_mask_odd_array[] = {0x01, 0x03, 0x03, 0x07, 0x07, 0x0f, 0x0f};
static const s8 kasan_track_mask_even_array[] = {-1,  0x01,  -1,  0x03,  -1,  0x07,  -1, 0x0f};
static s8 kasan_track_mask_odd(size_t size)
{
	return kasan_track_mask_odd_array[size - 1];
}

static s8 kasan_track_mask_even(size_t size)
{
	return kasan_track_mask_even_array[size - 1];
}

/* check with addr do not cross 8(shadow size)-byte boundary */
static __always_inline bool _memory_is_tracked(const void *addr, size_t size)
{
	s8 mask;
	u8 offset = (unsigned long)addr & KASAN_GRANULE_MASK;

	if ((unsigned long)addr & 0x01)
		mask = kasan_track_mask_odd(size);
	else
		mask = kasan_track_mask_even(size);

	return unlikely(get_track_value(addr) & (mask << (offset >> 1)));
}

static __always_inline bool memory_is_tracked_1(const void *addr)
{
	u8 last_accessible_byte = (unsigned long)addr & KASAN_GRANULE_MASK;

	return unlikely(get_track_value(addr) & (0x01 << (last_accessible_byte >> 1)));
}

static __always_inline bool memory_is_tracked_2_4_8(const void *addr, size_t size)
{
	/*
	 * Access crosses 8(shadow size)-byte boundary. Such access maps
	 * into 2 shadow bytes, so we need to check them both.
	 */
	if (unlikely((((unsigned long)addr + size - 1) & KASAN_GRANULE_MASK) < size - 1)) {
		u8 part = (unsigned long)addr & KASAN_GRANULE_MASK;

		part = 8 - part;
		return ((unlikely(get_track_value(addr)) && _memory_is_tracked(addr, part)) ||
					_memory_is_tracked(addr + part, size - part));
	}

	return _memory_is_tracked(addr, size);
}

static __always_inline bool memory_is_tracked_16(const void *addr)
{
	/* Unaligned 16-bytes access maps into 3 shadow bytes. */
	if (unlikely(!IS_ALIGNED((unsigned long)addr, KASAN_GRANULE_SIZE))) {
		u8 part = (unsigned long)addr & KASAN_GRANULE_MASK;

		part = 8 - part;
		return ((unlikely(get_track_value(addr)) && _memory_is_tracked(addr, part)) ||
			_memory_is_tracked(addr + part, 8) ||
			_memory_is_tracked(addr + part + 8, 8 - part));
	}

	return unlikely(get_track_value(addr) || get_track_value(addr + 8));
}

static __always_inline unsigned long track_bytes_is_nonzero(const s8 *start,
					size_t size)
{
	while (size) {
		if (unlikely(to_track_value_1_byte(*start)))
			return (unsigned long)start;
		start++;
		size--;
	}

	return 0;
}

static __always_inline unsigned long track_val_is_nonzero(const void *start,
						const void *end)
{
	unsigned int words;
	unsigned long ret;
	unsigned int prefix = (unsigned long)start % 8;

	if (end - start <= 16)
		return track_bytes_is_nonzero(start, end - start);

	if (prefix) {
		prefix = 8 - prefix;
		ret = track_bytes_is_nonzero(start, prefix);
		if (unlikely(ret))
			return ret;
		start += prefix;
	}

	words = (end - start) / 8;
	while (words) {
		if (unlikely(to_track_value_8_byte(*(u64 *)start)))
			return track_bytes_is_nonzero(start, 8);
		start += 8;
		words--;
	}

	return track_bytes_is_nonzero(start, (end - start) % 8);
}

static __always_inline bool memory_is_tracked_n(const void *addr, size_t size)
{
	unsigned long ret;

	ret = track_val_is_nonzero(kasan_mem_to_shadow(addr),
			kasan_mem_to_shadow(addr + size - 1) + 1);

	if (unlikely(ret)) {
		const void *last_byte = addr + size - 1;
		s8 *last_shadow = (s8 *)kasan_mem_to_shadow(last_byte);

		if (unlikely(ret != (unsigned long)last_shadow ||
				_memory_is_tracked(
				(void *)((unsigned long)last_byte & ~KASAN_GRANULE_MASK),
				((unsigned long)last_byte & KASAN_GRANULE_MASK) + 1)))
			return true;
	}
	return false;
}

static __always_inline bool memory_is_tracked(const void *addr, size_t size)
{
	if (__builtin_constant_p(size)) {
		switch (size) {
		case 1:
			return memory_is_tracked_1(addr);
		case 2:
		case 4:
		case 8:
			return memory_is_tracked_2_4_8(addr, size);
		case 16:
			return memory_is_tracked_16(addr);
		default:
			BUILD_BUG();
		}
	}

	return memory_is_tracked_n(addr, size);
}

/* deal with addr do not cross 8(shadow size)-byte boundary */
static void __kasan_track_memory(const void *shadow_addr, size_t offset, size_t size)
{
	s8 mask;

	if ((offset & 0x01) || (size & 0x01))
		mask = kasan_track_mask_odd(size);
	else
		mask = kasan_track_mask_even(size);
	offset = offset >> 1;
	*(s8 *)shadow_addr |= mask << (KASAN_TRACK_VALUE_OFFSET + offset);
}

static void _kasan_track_memory(const void *addr, size_t size)
{
	unsigned int words;
	const void *start = kasan_mem_to_shadow(addr);
	unsigned int prefix = (unsigned long)addr % 8;

	if (prefix) {
		unsigned int tmp_size = (unsigned int)size;

		tmp_size = min(8 - prefix, tmp_size);
		__kasan_track_memory(start, prefix, tmp_size);
		start++;
		size -= tmp_size;
	}

	words = size / 8;
	while (words) {
		__kasan_track_memory(start, 0, 8);
		start++;
		words--;
	}

	if (size % 8)
		__kasan_track_memory(start, 0, size % 8);
}

static inline bool is_cpu_entry_area_addr(unsigned long addr)
{
	return ((addr >= CPU_ENTRY_AREA_BASE) &&
		(addr < CPU_ENTRY_AREA_BASE + CPU_ENTRY_AREA_MAP_SIZE));
}

static inline bool is_kernel_text_data(unsigned long addr)
{
	return ((addr >= (unsigned long)_stext) && (addr < (unsigned long)_end));
}

static bool can_track(unsigned long addr)
{
	if (!virt_addr_valid(addr) &&
		!is_module_address(addr) &&
#ifdef CONFIG_KASAN_VMALLOC
		!is_vmalloc_addr((const void *)addr) &&
#endif
		!is_cpu_entry_area_addr(addr) &&
		!is_kernel_text_data(addr)
	)
		return false;

	return true;
}

int kasan_track_memory(const void *addr, size_t size)
{
	if (!kasan_arch_is_ready())
		return -EINVAL;

	if (unlikely(size == 0))
		return -EINVAL;

	if (unlikely(addr + size < addr))
		return -EINVAL;

	if (unlikely(!addr_has_metadata(addr)))
		return -EINVAL;

	if (likely(memory_is_poisoned(addr, size)))
		return -EINVAL;

	if (!can_track((unsigned long)addr))
		return -EINVAL;

	_kasan_track_memory(addr, size);
	return 0;
}
EXPORT_SYMBOL(kasan_track_memory);

/* deal with addr do not cross 8(shadow size)-byte boundary */
static void __kasan_untrack_memory(const void *shadow_addr, size_t offset, size_t size)
{
	s8 mask;

	if (size % 0x01) {
		offset = (offset - 1) >> 1;
		mask = kasan_track_mask_odd(size);
		/*
		 * SIZE is odd, which means we may clear someone else's tracking flags of
		 * nearby tracked memory.
		 */
		pr_info("It's possible to clear someone else's tracking flags\n");
	} else {
		offset = offset >> 1;
		mask = kasan_track_mask_even(size);
	}
	*(s8 *)shadow_addr &= ~(mask << (KASAN_TRACK_VALUE_OFFSET + offset));
}

static void _kasan_untrack_memory(const void *addr, size_t size)
{
	unsigned int words;
	const void *start = kasan_mem_to_shadow(addr);
	unsigned int prefix = (unsigned long)addr % 8;

	if (prefix) {
		unsigned int tmp_size = (unsigned int)size;

		tmp_size = min(8 - prefix, tmp_size);
		__kasan_untrack_memory(start, prefix, tmp_size);
		start++;
		size -= tmp_size;
	}

	words = size / 8;
	while (words) {
		__kasan_untrack_memory(start, 0, 8);
		start++;
		words--;
	}

	if (size % 8)
		__kasan_untrack_memory(start, 0, size % 8);
}

int kasan_untrack_memory(const void *addr, size_t size)
{
	if (!kasan_arch_is_ready())
		return -EINVAL;

	if (unlikely(size == 0))
		return -EINVAL;

	if (unlikely(addr + size < addr))
		return -EINVAL;

	if (unlikely(!addr_has_metadata(addr)))
		return -EINVAL;

	if (likely(memory_is_poisoned(addr, size)))
		return -EINVAL;

	if (!can_track((unsigned long)addr))
		return -EINVAL;

	_kasan_untrack_memory(addr, size);
	return 0;
}
EXPORT_SYMBOL(kasan_untrack_memory);
#endif

static __always_inline bool check_region_inline(const void *addr,
						size_t size, bool write,
						unsigned long ret_ip)
{
	if (!kasan_arch_is_ready())
		return true;

	if (unlikely(size == 0))
		return true;

	if (unlikely(addr + size < addr))
		return !kasan_report(addr, size, write, ret_ip);

	if (unlikely(!addr_has_metadata(addr)))
		return !kasan_report(addr, size, write, ret_ip);

	if ((likely(!memory_is_poisoned(addr, size))) &&
		(!write || likely(!memory_is_tracked(addr, size))))
		return true;

	return !kasan_report(addr, size, write, ret_ip);
}

bool kasan_check_range(const void *addr, size_t size, bool write,
					unsigned long ret_ip)
{
	return check_region_inline(addr, size, write, ret_ip);
}

bool kasan_byte_accessible(const void *addr)
{
	s8 shadow_byte;

	if (!kasan_arch_is_ready())
		return true;

	shadow_byte = (s8)to_shadow_value_1_byte(READ_ONCE(*(s8 *)kasan_mem_to_shadow(addr)));

	return shadow_byte >= 0 && shadow_byte < KASAN_GRANULE_SIZE;
}

void kasan_cache_shrink(struct kmem_cache *cache)
{
	kasan_quarantine_remove_cache(cache);
}

void kasan_cache_shutdown(struct kmem_cache *cache)
{
	if (!__kmem_cache_empty(cache))
		kasan_quarantine_remove_cache(cache);
}

static void register_global(struct kasan_global *global)
{
	size_t aligned_size = round_up(global->size, KASAN_GRANULE_SIZE);

	kasan_unpoison(global->beg, global->size, false);

	kasan_poison(global->beg + aligned_size,
		     global->size_with_redzone - aligned_size,
		     KASAN_GLOBAL_REDZONE, false);
}

void __asan_register_globals(void *ptr, ssize_t size)
{
	int i;
	struct kasan_global *globals = ptr;

	for (i = 0; i < size; i++)
		register_global(&globals[i]);
}
EXPORT_SYMBOL(__asan_register_globals);

void __asan_unregister_globals(void *ptr, ssize_t size)
{
}
EXPORT_SYMBOL(__asan_unregister_globals);

#define DEFINE_ASAN_LOAD_STORE(size)					\
	void __asan_load##size(void *addr)				\
	{								\
		check_region_inline(addr, size, false, _RET_IP_);	\
	}								\
	EXPORT_SYMBOL(__asan_load##size);				\
	__alias(__asan_load##size)					\
	void __asan_load##size##_noabort(void *);			\
	EXPORT_SYMBOL(__asan_load##size##_noabort);			\
	void __asan_store##size(void *addr)				\
	{								\
		check_region_inline(addr, size, true, _RET_IP_);	\
	}								\
	EXPORT_SYMBOL(__asan_store##size);				\
	__alias(__asan_store##size)					\
	void __asan_store##size##_noabort(void *);			\
	EXPORT_SYMBOL(__asan_store##size##_noabort)

DEFINE_ASAN_LOAD_STORE(1);
DEFINE_ASAN_LOAD_STORE(2);
DEFINE_ASAN_LOAD_STORE(4);
DEFINE_ASAN_LOAD_STORE(8);
DEFINE_ASAN_LOAD_STORE(16);

void __asan_loadN(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__asan_loadN);

__alias(__asan_loadN)
void __asan_loadN_noabort(void *, ssize_t);
EXPORT_SYMBOL(__asan_loadN_noabort);

void __asan_storeN(void *addr, ssize_t size)
{
	kasan_check_range(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__asan_storeN);

__alias(__asan_storeN)
void __asan_storeN_noabort(void *, ssize_t);
EXPORT_SYMBOL(__asan_storeN_noabort);

/* to shut up compiler complaints */
void __asan_handle_no_return(void) {}
EXPORT_SYMBOL(__asan_handle_no_return);

/* Emitted by compiler to poison alloca()ed objects. */
void __asan_alloca_poison(void *addr, ssize_t size)
{
	size_t rounded_up_size = round_up(size, KASAN_GRANULE_SIZE);
	size_t padding_size = round_up(size, KASAN_ALLOCA_REDZONE_SIZE) -
			rounded_up_size;
	size_t rounded_down_size = round_down(size, KASAN_GRANULE_SIZE);

	const void *left_redzone = (const void *)(addr -
			KASAN_ALLOCA_REDZONE_SIZE);
	const void *right_redzone = (const void *)(addr + rounded_up_size);

	WARN_ON(!IS_ALIGNED((unsigned long)addr, KASAN_ALLOCA_REDZONE_SIZE));

	kasan_unpoison((const void *)(addr + rounded_down_size),
			size - rounded_down_size, false);
	kasan_poison(left_redzone, KASAN_ALLOCA_REDZONE_SIZE,
		     KASAN_ALLOCA_LEFT, false);
	kasan_poison(right_redzone, padding_size + KASAN_ALLOCA_REDZONE_SIZE,
		     KASAN_ALLOCA_RIGHT, false);
}
EXPORT_SYMBOL(__asan_alloca_poison);

/* Emitted by compiler to unpoison alloca()ed areas when the stack unwinds. */
void __asan_allocas_unpoison(void *stack_top, ssize_t stack_bottom)
{
	if (unlikely(!stack_top || stack_top > (void *)stack_bottom))
		return;

	kasan_unpoison(stack_top, (void *)stack_bottom - stack_top, false);
}
EXPORT_SYMBOL(__asan_allocas_unpoison);

/* Emitted by the compiler to [un]poison local variables. */
#define DEFINE_ASAN_SET_SHADOW(byte) \
	void __asan_set_shadow_##byte(const void *addr, ssize_t size)	\
	{								\
		__memset((void *)addr, 0x##byte, size);			\
	}								\
	EXPORT_SYMBOL(__asan_set_shadow_##byte)

DEFINE_ASAN_SET_SHADOW(00);
DEFINE_ASAN_SET_SHADOW(f1);
DEFINE_ASAN_SET_SHADOW(f2);
DEFINE_ASAN_SET_SHADOW(f3);
DEFINE_ASAN_SET_SHADOW(f5);
DEFINE_ASAN_SET_SHADOW(f8);

/* Only allow cache merging when no per-object metadata is present. */
slab_flags_t kasan_never_merge(void)
{
	if (!kasan_requires_meta())
		return 0;
	return SLAB_KASAN;
}

/*
 * Adaptive redzone policy taken from the userspace AddressSanitizer runtime.
 * For larger allocations larger redzones are used.
 */
static inline unsigned int optimal_redzone(unsigned int object_size)
{
	return
		object_size <= 64        - 16   ? 16 :
		object_size <= 128       - 32   ? 32 :
		object_size <= 512       - 64   ? 64 :
		object_size <= 4096      - 128  ? 128 :
		object_size <= (1 << 14) - 256  ? 256 :
		object_size <= (1 << 15) - 512  ? 512 :
		object_size <= (1 << 16) - 1024 ? 1024 : 2048;
}

void kasan_cache_create(struct kmem_cache *cache, unsigned int *size,
			  slab_flags_t *flags)
{
	unsigned int ok_size;
	unsigned int optimal_size;
	unsigned int rem_free_meta_size;
	unsigned int orig_alloc_meta_offset;

	if (!kasan_requires_meta())
		return;

	/*
	 * SLAB_KASAN is used to mark caches that are sanitized by KASAN
	 * and that thus have per-object metadata.
	 * Currently this flag is used in two places:
	 * 1. In slab_ksize() to account for per-object metadata when
	 *    calculating the size of the accessible memory within the object.
	 * 2. In slab_common.c via kasan_never_merge() to prevent merging of
	 *    caches with per-object metadata.
	 */
	*flags |= SLAB_KASAN;

	ok_size = *size;

	/* Add alloc meta into the redzone. */
	cache->kasan_info.alloc_meta_offset = *size;
	*size += sizeof(struct kasan_alloc_meta);

	/* If alloc meta doesn't fit, don't add it. */
	if (*size > KMALLOC_MAX_SIZE) {
		cache->kasan_info.alloc_meta_offset = 0;
		*size = ok_size;
		/* Continue, since free meta might still fit. */
	}

	ok_size = *size;
	orig_alloc_meta_offset = cache->kasan_info.alloc_meta_offset;

	/*
	 * Store free meta in the redzone when it's not possible to store
	 * it in the object. This is the case when:
	 * 1. Object is SLAB_TYPESAFE_BY_RCU, which means that it can
	 *    be touched after it was freed, or
	 * 2. Object has a constructor, which means it's expected to
	 *    retain its content until the next allocation.
	 */
	if ((cache->flags & SLAB_TYPESAFE_BY_RCU) || cache->ctor) {
		cache->kasan_info.free_meta_offset = *size;
		*size += sizeof(struct kasan_free_meta);
		goto free_meta_added;
	}

	/*
	 * Otherwise, if the object is large enough to contain free meta,
	 * store it within the object.
	 */
	if (sizeof(struct kasan_free_meta) <= cache->object_size) {
		/* cache->kasan_info.free_meta_offset = 0 is implied. */
		goto free_meta_added;
	}

	/*
	 * For smaller objects, store the beginning of free meta within the
	 * object and the end in the redzone. And thus shift the location of
	 * alloc meta to free up space for free meta.
	 * This is only possible when slub_debug is disabled, as otherwise
	 * the end of free meta will overlap with slub_debug metadata.
	 */
	if (!__slub_debug_enabled()) {
		rem_free_meta_size = sizeof(struct kasan_free_meta) -
							cache->object_size;
		*size += rem_free_meta_size;
		if (cache->kasan_info.alloc_meta_offset != 0)
			cache->kasan_info.alloc_meta_offset += rem_free_meta_size;
		goto free_meta_added;
	}

	/*
	 * If the object is small and slub_debug is enabled, store free meta
	 * in the redzone after alloc meta.
	 */
	cache->kasan_info.free_meta_offset = *size;
	*size += sizeof(struct kasan_free_meta);

free_meta_added:
	/* If free meta doesn't fit, don't add it. */
	if (*size > KMALLOC_MAX_SIZE) {
		cache->kasan_info.free_meta_offset = KASAN_NO_FREE_META;
		cache->kasan_info.alloc_meta_offset = orig_alloc_meta_offset;
		*size = ok_size;
	}

	/* Calculate size with optimal redzone. */
	optimal_size = cache->object_size + optimal_redzone(cache->object_size);
	/* Limit it with KMALLOC_MAX_SIZE. */
	if (optimal_size > KMALLOC_MAX_SIZE)
		optimal_size = KMALLOC_MAX_SIZE;
	/* Use optimal size if the size with added metas is not large enough. */
	if (*size < optimal_size)
		*size = optimal_size;
}

struct kasan_alloc_meta *kasan_get_alloc_meta(struct kmem_cache *cache,
					      const void *object)
{
	if (!cache->kasan_info.alloc_meta_offset)
		return NULL;
	return (void *)object + cache->kasan_info.alloc_meta_offset;
}

struct kasan_free_meta *kasan_get_free_meta(struct kmem_cache *cache,
					    const void *object)
{
	BUILD_BUG_ON(sizeof(struct kasan_free_meta) > 32);
	if (cache->kasan_info.free_meta_offset == KASAN_NO_FREE_META)
		return NULL;
	return (void *)object + cache->kasan_info.free_meta_offset;
}

void kasan_init_object_meta(struct kmem_cache *cache, const void *object)
{
	struct kasan_alloc_meta *alloc_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (alloc_meta) {
		/* Zero out alloc meta to mark it as invalid. */
		__memset(alloc_meta, 0, sizeof(*alloc_meta));

		/*
		 * Prepare the lock for saving auxiliary stack traces.
		 * Temporarily disable KASAN bug reporting to allow instrumented
		 * raw_spin_lock_init to access aux_lock, which resides inside
		 * of a redzone.
		 */
		kasan_disable_current();
		raw_spin_lock_init(&alloc_meta->aux_lock);
		kasan_enable_current();
	}

	/*
	 * Explicitly marking free meta as invalid is not required: the shadow
	 * value for the first 8 bytes of a newly allocated object is not
	 * KASAN_SLAB_FREE_META.
	 */
}

static void release_alloc_meta(struct kasan_alloc_meta *meta)
{
	/* Evict the stack traces from stack depot. */
	stack_depot_put(meta->alloc_track.stack);
	stack_depot_put(meta->aux_stack[0]);
	stack_depot_put(meta->aux_stack[1]);

	/*
	 * Zero out alloc meta to mark it as invalid but keep aux_lock
	 * initialized to avoid having to reinitialize it when another object
	 * is allocated in the same slot.
	 */
	__memset(&meta->alloc_track, 0, sizeof(meta->alloc_track));
	__memset(meta->aux_stack, 0, sizeof(meta->aux_stack));
}

static void release_free_meta(const void *object, struct kasan_free_meta *meta)
{
	/* Check if free meta is valid. */
	if (*(u8 *)kasan_mem_to_shadow(object) != KASAN_SLAB_FREE_META)
		return;

	/* Evict the stack trace from the stack depot. */
	stack_depot_put(meta->free_track.stack);

	/* Mark free meta as invalid. */
	*(u8 *)kasan_mem_to_shadow(object) = KASAN_SLAB_FREE;
}

void kasan_release_object_meta(struct kmem_cache *cache, const void *object)
{
	struct kasan_alloc_meta *alloc_meta;
	struct kasan_free_meta *free_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (alloc_meta)
		release_alloc_meta(alloc_meta);

	free_meta = kasan_get_free_meta(cache, object);
	if (free_meta)
		release_free_meta(object, free_meta);
}

size_t kasan_metadata_size(struct kmem_cache *cache, bool in_object)
{
	struct kasan_cache *info = &cache->kasan_info;

	if (!kasan_requires_meta())
		return 0;

	if (in_object)
		return (info->free_meta_offset ?
			0 : sizeof(struct kasan_free_meta));
	else
		return (info->alloc_meta_offset ?
			sizeof(struct kasan_alloc_meta) : 0) +
			((info->free_meta_offset &&
			info->free_meta_offset != KASAN_NO_FREE_META) ?
			sizeof(struct kasan_free_meta) : 0);
}

static void __kasan_record_aux_stack(void *addr, depot_flags_t depot_flags)
{
	struct slab *slab = kasan_addr_to_slab(addr);
	struct kmem_cache *cache;
	struct kasan_alloc_meta *alloc_meta;
	void *object;
	depot_stack_handle_t new_handle, old_handle;
	unsigned long flags;

	if (is_kfence_address(addr) || !slab)
		return;

	cache = slab->slab_cache;
	object = nearest_obj(cache, slab, addr);
	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (!alloc_meta)
		return;

	new_handle = kasan_save_stack(0, depot_flags);

	/*
	 * Temporarily disable KASAN bug reporting to allow instrumented
	 * spinlock functions to access aux_lock, which resides inside of a
	 * redzone.
	 */
	kasan_disable_current();
	raw_spin_lock_irqsave(&alloc_meta->aux_lock, flags);
	old_handle = alloc_meta->aux_stack[1];
	alloc_meta->aux_stack[1] = alloc_meta->aux_stack[0];
	alloc_meta->aux_stack[0] = new_handle;
	raw_spin_unlock_irqrestore(&alloc_meta->aux_lock, flags);
	kasan_enable_current();

	stack_depot_put(old_handle);
}

void kasan_record_aux_stack(void *addr)
{
	return __kasan_record_aux_stack(addr,
			STACK_DEPOT_FLAG_CAN_ALLOC | STACK_DEPOT_FLAG_GET);
}

void kasan_record_aux_stack_noalloc(void *addr)
{
	return __kasan_record_aux_stack(addr, STACK_DEPOT_FLAG_GET);
}

void kasan_save_alloc_info(struct kmem_cache *cache, void *object, gfp_t flags)
{
	struct kasan_alloc_meta *alloc_meta;

	alloc_meta = kasan_get_alloc_meta(cache, object);
	if (!alloc_meta)
		return;

	/* Evict previous stack traces (might exist for krealloc or mempool). */
	release_alloc_meta(alloc_meta);

	kasan_save_track(&alloc_meta->alloc_track, flags);
}

void kasan_save_free_info(struct kmem_cache *cache, void *object)
{
	struct kasan_free_meta *free_meta;

	free_meta = kasan_get_free_meta(cache, object);
	if (!free_meta)
		return;

	/* Evict previous stack trace (might exist for mempool). */
	release_free_meta(object, free_meta);

	kasan_save_track(&free_meta->free_track, 0);

	/* Mark free meta as valid. */
	*(u8 *)kasan_mem_to_shadow(object) = KASAN_SLAB_FREE_META;
}
