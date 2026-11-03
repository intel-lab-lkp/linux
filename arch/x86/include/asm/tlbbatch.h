/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_X86_TLBBATCH_H
#define _ARCH_X86_TLBBATCH_H

#include <linux/cpumask.h>

struct mm_struct;

#define ARCH_TLBBATCH_MAX_MMS	8

struct arch_tlbflush_unmap_batch {
	/*
	 * Each bit set is a CPU that potentially has a TLB entry for one of
	 * the PFNs being flushed..
	 */
	struct cpumask cpumask;

	/*
	 * Unique mm_struct pointers unmapped in this batch. Used only for
	 * pointer equality against per-CPU loaded_mm in arch_tlbbatch_flush()
	 * and never dereferenced there. nr_mms > ARCH_TLBBATCH_MAX_MMS
	 * indicates overflow.
	 */
	struct mm_struct *mms[ARCH_TLBBATCH_MAX_MMS];
	unsigned short nr_mms;

	/*
	 * Set if pages were unmapped from any MM, even one that does not
	 * have active CPUs in its cpumask.
	 */
	bool unmapped_pages;
};

#endif /* _ARCH_X86_TLBBATCH_H */
