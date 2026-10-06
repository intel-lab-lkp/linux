/* SPDX-License-Identifier: GPL-2.0 */
/*
 * x86 KMSAN support.
 *
 * Copyright (C) 2022, Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 */

#ifndef _ASM_X86_KMSAN_H
#define _ASM_X86_KMSAN_H

#ifndef MODULE

#include <asm/cpu_entry_area.h>
#include <asm/processor.h>
#include <linux/cpumask.h>
#include <linux/mmzone.h>

DECLARE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_shadow);
DECLARE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_origin);

/*
 * Functions below are declared in the header to make sure they are inlined.
 * They all are called from kmsan_get_metadata() for every memory access in
 * the kernel, so speed is important here.
 */

/*
 * Compute metadata addresses for the CPU entry area on x86.
 */
static inline void *arch_kmsan_get_meta_or_null(void *addr, bool is_origin)
{
	unsigned long addr64 = (unsigned long)addr;
	char *metadata_array;
	unsigned long off;
	unsigned int cpu;

	if ((addr64 < CPU_ENTRY_AREA_BASE) ||
	    (addr64 >= (CPU_ENTRY_AREA_BASE + CPU_ENTRY_AREA_MAP_SIZE)))
		return NULL;

	/*
	 * The per-CPU entry areas are not necessarily laid out in CPU order
	 * (see init_cea_offsets()), so find the area containing @addr instead
	 * of computing the CPU from the offset.
	 */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		if (!cpu_possible(cpu))
			continue;
		off = addr64 - (unsigned long)get_cpu_entry_area(cpu);
		if (off >= CPU_ENTRY_AREA_SIZE)
			continue;
		metadata_array = is_origin ? cpu_entry_area_origin :
					     cpu_entry_area_shadow;
		return &per_cpu(metadata_array[off], cpu);
	}
	return NULL;
}

/*
 * Taken from arch/x86/mm/physaddr.h to avoid using an instrumented version.
 */
static inline bool kmsan_phys_addr_valid(unsigned long addr)
{
	if (IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT))
		return !(addr >> boot_cpu_data.x86_phys_bits);
	else
		return true;
}

/*
 * Same as pfn_valid(), but without rcu_read_lock_sched(). That one calls into
 * instrumented code: preempt_count_add() with CONFIG_DEBUG_PREEMPT or
 * CONFIG_TRACE_PREEMPT_TOGGLE, lock_acquire() with CONFIG_DEBUG_LOCK_ALLOC.
 * Instrumented code looks up metadata itself, so calling it from the metadata
 * lookup can recurse; with CONFIG_DEBUG_PREEMPT it does, until the stack
 * overflows.
 *
 * Disabling preemption still makes this an RCU-sched read-side critical
 * section. Preemption is re-enabled without a reschedule, as before, to
 * avoid entering the scheduler from the metadata lookup.
 */
static inline bool kmsan_pfn_valid(unsigned long pfn)
{
	struct mem_section *ms;
	bool ret;

	if (PHYS_PFN(PFN_PHYS(pfn)) != pfn)
		return false;

	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return false;
	ms = __pfn_to_section(pfn);

	preempt_disable_notrace();
	ret = valid_section(ms) &&
	      (early_section(ms) || pfn_section_valid(ms, pfn));
	preempt_enable_no_resched_notrace();

	return ret;
}

/*
 * Taken from arch/x86/mm/physaddr.c to avoid using an instrumented version.
 */
static inline bool kmsan_virt_addr_valid(void *addr)
{
	unsigned long x = (unsigned long)addr;
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	if (unlikely(x > y)) {
		x = y + phys_base;

		if (y >= KERNEL_IMAGE_SIZE)
			return false;
	} else {
		x = y + (__START_KERNEL_map - PAGE_OFFSET);

		/* carry flag will be set if starting x was >= PAGE_OFFSET */
		if ((x > y) || !kmsan_phys_addr_valid(x))
			return false;
	}

	return kmsan_pfn_valid(x >> PAGE_SHIFT);
}

#endif /* !MODULE */

#endif /* _ASM_X86_KMSAN_H */
