/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_HYGON_NODE_H
#define _ASM_X86_HYGON_NODE_H

#include <linux/errno.h>
#include <linux/types.h>

#include <asm/processor.h>
#include <asm/topology.h>

struct pci_dev;

#define HYGON_MAX_SOCKETS	8

#ifdef CONFIG_AMD_NODE

/**
 * hygon_node_num() - total node entries (CDD + IOD)
 *
 * Hygon Fam18h enumerates nodes by walking DF misc (F3) devices rather than
 * using the fixed 00:18..1f scheme used by AMD. Returns the total count of
 * CDD and IOD nodes, which sizes amd_nb[] and amd_roots[].
 */
u16 hygon_node_num(void);

/**
 * hygon_cdd_num() - number of compute dies (CDD)
 *
 * DF IDs >= 4 represent compute dies (CDD). DF IDs < 4 represent IO dies
 * (IOD). EDAC instances must be sized by CDD count, not total node count,
 * because IOD nodes have no UMC.
 */
u16 hygon_cdd_num(void);

/**
 * hygon_socket_num() - number of sockets
 */
u16 hygon_socket_num(void);

/**
 * hygon_f18h_model() - return Hygon Fam18h model byte, or 0 if not Hygon Fam18h
 */
u8 hygon_f18h_model(void);

/**
 * hygon_node_get_func() - get PCI dev for a Hygon node DF function
 * @node: logical node index (0..hygon_node_num()-1)
 * @func: PCI function number (3 for DF misc, 4 for DF link)
 *
 * Returns a reference-counted pci_dev pointer. Caller must call
 * pci_dev_put() when done.
 */
struct pci_dev *hygon_node_get_func(u16 node, u8 func);

/**
 * hygon_node_socket() - get socket ID for a logical node
 * @node: logical node index (0..hygon_node_num()-1)
 *
 * Returns the hardware socket ID for @node (0..hygon_socket_num()-1).
 * Used by amd_smn_init() to assign per-socket SMN root devices to the
 * per-node amd_roots[] array. Returns U8_MAX on error.
 */
u8 hygon_node_socket(u16 node);

/**
 * hygon_get_dfid() - read DF ID for a Hygon DF misc device
 * @misc: PCI dev for DF misc (function 3)
 * @dfid: output DF ID
 */
int hygon_get_dfid(struct pci_dev *misc, u8 *dfid);

/**
 * hygon_cpu_to_logical_node() - map CPU to dense logical node ID (O(1))
 * @cpu: CPU index
 *
 * Hygon Fam18h exposes sparse physical node IDs (CPUID 8000001E[7:0]).
 * This function translates the per-CPU physical node ID into a contiguous
 * logical node ID (0..hygon_cdd_num()-1) that aligns with EDAC instance
 * numbering and amd_nb[] indexing.
 *
 * Return: logical node id on success, negative errno on failure.
 */
int hygon_cpu_to_logical_node(unsigned int cpu);

#else /* !CONFIG_AMD_NODE */
static inline u16 hygon_node_num(void)
{
	return 0;
}

static inline u16 hygon_cdd_num(void)
{
	return 0;
}

static inline u16 hygon_socket_num(void)
{
	return 0;
}

static inline u8 hygon_f18h_model(void)
{
	return 0;
}

static inline struct pci_dev *hygon_node_get_func(u16 node, u8 func)
{
	return NULL;
}

static inline u8 hygon_node_socket(u16 node)
{
	return U8_MAX;
}

static inline int hygon_get_dfid(struct pci_dev *misc, u8 *dfid)
{
	return -ENODEV;
}

static inline int hygon_cpu_to_logical_node(unsigned int cpu)
{
	return -ENODEV;
}
#endif /* CONFIG_AMD_NODE */

/**
 * hygon_f18h_m4h() - check Hygon family 18h model 4h..fh
 */
static inline bool hygon_f18h_m4h(void)
{
	u8 m = hygon_f18h_model();

	return m >= 0x4 && m <= 0xf;
}

/**
 * hygon_f18h_m10h() - check Hygon family 18h model 10h..1fh
 */
static inline bool hygon_f18h_m10h(void)
{
	u8 m = hygon_f18h_model();

	return m >= 0x10;
}


#endif /* _ASM_X86_HYGON_NODE_H */
