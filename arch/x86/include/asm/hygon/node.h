/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_HYGON_NODE_H
#define _ASM_X86_HYGON_NODE_H

#include <linux/errno.h>
#include <linux/types.h>

struct pci_dev;

#define HYGON_MAX_SOCKETS	8

#ifdef CONFIG_HYGON_NODE

/**
 * hygon_cdd_num() - number of compute dies (CDD)
 *
 * DF IDs >= 4 represent compute dies (CDD). DF IDs < 4 represent IO dies
 * (IOD). EDAC instances must be sized by CDD count, not total node count,
 * because IOD nodes have no UMC.
 */
u16 hygon_cdd_num(void);

/**
 * hygon_f18h_model() - return Hygon Fam18h model byte, or 0 if not Hygon Fam18h
 */
u8 hygon_f18h_model(void);

/**
 * hygon_get_dfid() - read DF ID for a Hygon DF misc device
 * @misc: PCI dev for DF misc (function 3)
 * @dfid: output DF ID
 */
int hygon_get_dfid(struct pci_dev *misc, u8 *dfid);

/**
 * hygon_cpu_to_df_node() - map CPU to dense DF node index (O(1))
 * @cpu: CPU index
 *
 * Hygon Fam18h exposes sparse physical node IDs (CPUID 8000001E[7:0]).
 * This function translates the per-CPU physical node ID into a contiguous
 * DF node index (0..hygon_cdd_num()-1) that aligns with EDAC instance
 * numbering and amd_nb[] indexing.
 *
 * Return: DF node index on success, negative errno on failure.
 */
int hygon_cpu_to_df_node(unsigned int cpu);

#else /* !CONFIG_HYGON_NODE */

static inline u16 hygon_cdd_num(void)
{
	return 0;
}

static inline u8 hygon_f18h_model(void)
{
	return 0;
}

static inline int hygon_get_dfid(struct pci_dev *misc, u8 *dfid)
{
	return -ENODEV;
}

static inline int hygon_cpu_to_df_node(unsigned int cpu)
{
	return -ENODEV;
}
#endif /* CONFIG_HYGON_NODE */

/**
 * hygon_f18h_model_in_range() - check if Hygon Fam18h model is within [first, last]
 * @first: lower bound (inclusive)
 * @last:  upper bound (inclusive)
 *
 * Returns false on non-Hygon-Fam18h systems (hygon_f18h_model() returns 0).
 */
static inline bool hygon_f18h_model_in_range(u8 first, u8 last)
{
	u8 m = hygon_f18h_model();

	return m >= first && m <= last;
}

#endif /* _ASM_X86_HYGON_NODE_H */
