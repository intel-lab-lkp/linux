/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hygon Family 0x18 Data Fabric node enumeration API
 *
 * This header exposes the Hygon Fam18h DF node enumeration and DF
 * function access primitives consumers (EDAC, MCE decode, ATL, etc.)
 * need.
 */
#ifndef _ASM_X86_HYGON_NODE_H
#define _ASM_X86_HYGON_NODE_H

#include <linux/errno.h>
#include <linux/processor.h>
#include <linux/types.h>

struct pci_dev;

/* DF function indices supported by hygon_node_get_func(). */
#define HYGON_DF_F3	3	/* misc */
#define HYGON_DF_F4	4	/* link */

/* Hygon compute dies (CDD) start at DFID 4; IO dies occupy DFIDs 0-3. */
#define HYGON_CDD_DFID_BASE	4

/**
 * enum hygon_node_type - DF node type, derived from DFID
 * @HYGON_NODE_IOD: I/O die (DFID < HYGON_CDD_DFID_BASE); no UMC.
 * @HYGON_NODE_CDD: compute die (DFID >= HYGON_CDD_DFID_BASE); hosts
 *                  CPU cores and UMC controllers.
 */
enum hygon_node_type {
	HYGON_NODE_IOD = 0,
	HYGON_NODE_CDD = 1,
};

/**
 * struct hygon_node_info - identity snapshot for a DF node
 * @socket_id: physical socket ID, F1x200[30:28]
 * @dfid:      Data Fabric ID, model-dependent source
 * @type:      HYGON_NODE_CDD or HYGON_NODE_IOD
 */
struct hygon_node_info {
	u8			socket_id;
	u8			dfid;
	enum hygon_node_type	type;
};

#ifdef CONFIG_HYGON_NODE

/**
 * hygon_node_num() - total number of DF nodes (CDD + IOD)
 *
 * Return: total node count, or 0 if the cache is not ready or this is
 * not a Hygon Fam18h platform.  Use this as the upper bound when
 * iterating over DF nodes or indexing SMN access.
 */
u16 hygon_node_num(void);

/**
 * hygon_cdd_num() - number of compute dies (CDD)
 *
 * DFID >= 4 marks a compute die (CDD).  DFID < 4 marks an I/O die
 * (IOD) which has no UMC.  Use this count to size EDAC memory-controller
 * instances and to iterate over UMC-bearing nodes.
 *
 * Return: CDD count, or 0 if the cache is not ready.
 */
u16 hygon_cdd_num(void);

/**
 * hygon_node_get_info() - read identity snapshot for a DF node
 * @node: DF node index in [0, hygon_node_num())
 * @info: output structure (socket_id, dfid, type)
 *
 * Copies all immutable identity fields in one call so callers that
 * need to make decisions on multiple fields (e.g. "if CDD then read
 * DFID and compute UMC base") do not need multiple cache lookups.
 *
 * Return: 0 on success, -EINVAL if @node is out of range or @info is
 * NULL, -ENODEV if the cache is not ready.
 */
int hygon_node_get_info(u16 node, struct hygon_node_info *info);

/**
 * hygon_cpu_to_df_node() - map CPU to dense DF CDD index
 * @cpu: CPU index
 *
 * Hygon Fam18h exposes sparse physical node IDs via CPUID 8000001E[7:0].
 * This function translates the per-CPU physical node ID into a dense
 * DF CDD index in [0, hygon_cdd_num()).
 *
 * Return: DF CDD index on success, -EINVAL if @cpu is out of range,
 * -ENODEV if CPU-to-DF mapping is unsupported on this model or the
 * physical node ID does not map to a known DF node.
 */
int hygon_cpu_to_df_node(unsigned int cpu);

/**
 * hygon_node_get_func() - get DF function PCI device for a node
 * @node: DF node index in [0, hygon_node_num())
 * @func: HYGON_DF_F3 or HYGON_DF_F4
 *
 * Return: referenced pci_dev on success.  NULL if @node is out of
 * range, @func is unsupported, or the cache is not ready.  The
 * caller must release the reference with pci_dev_put().
 */
struct pci_dev *hygon_node_get_func(u16 node, u8 func);

/**
 * hygon_pci_dev_to_df_node() - find DF node owning the given PCI device
 * @pdev: PCI device on the same domain, bus and slot as one of the DF
 *        nodes (typically a sibling function of the DF misc device,
 *        e.g. a UMC channel)
 *
 * Looks up the DF node whose misc (F3) device shares the same PCI
 * domain, bus, and slot as @pdev.  Useful for drivers that hold a
 * per-PCI-function device and need the containing DF node index for
 * SMN access or other node-indexed operations.
 *
 * Return: DF node index in [0, hygon_node_num()) on success, -EINVAL
 * if @pdev is NULL, -ENODEV if no matching node is found or the cache
 * is not ready.
 */
int hygon_pci_dev_to_df_node(struct pci_dev *pdev);

/**
 * hygon_smn_read() - read a 32-bit value from a Hygon SMN address
 * @node: DF node index in [0, hygon_node_num())
 * @address: SMN address
 * @value: output value
 *
 * Return: 0 on success, -EINVAL if @value is NULL, -ENODEV if Hygon
 * SMN is not initialised or @node is out of range, or a negative
 * errno from the underlying PCI config access.
 */
int __must_check hygon_smn_read(u16 node, u32 address, u32 *value);

/**
 * hygon_smn_write() - write a 32-bit value to a Hygon SMN address
 * @node: DF node index in [0, hygon_node_num())
 * @address: SMN address
 * @value: value to write
 *
 * Return: 0 on success, -ENODEV if Hygon SMN is not initialised or
 * @node is out of range, or a negative errno from the underlying
 * PCI config access.
 */
int __must_check hygon_smn_write(u16 node, u32 address, u32 value);

#else /* !CONFIG_HYGON_NODE */

static inline u16 hygon_node_num(void)
{
	return 0;
}

static inline u16 hygon_cdd_num(void)
{
	return 0;
}

static inline int hygon_node_get_info(u16 node, struct hygon_node_info *info)
{
	return -ENODEV;
}

static inline int hygon_cpu_to_df_node(unsigned int cpu)
{
	return -ENODEV;
}

static inline struct pci_dev *hygon_node_get_func(u16 node, u8 func)
{
	return NULL;
}

static inline int hygon_pci_dev_to_df_node(struct pci_dev *pdev)
{
	return -ENODEV;
}

static inline int hygon_smn_read(u16 node, u32 address, u32 *value)
{
	return -ENODEV;
}

static inline int hygon_smn_write(u16 node, u32 address, u32 value)
{
	return -ENODEV;
}

#endif /* CONFIG_HYGON_NODE */

/* Inline helpers, available regardless of CONFIG_HYGON_NODE. */

/**
 * is_hygon_f18h() - true on Hygon Family 0x18 CPUs
 */
static inline bool is_hygon_f18h(void)
{
	return boot_cpu_data.x86_vendor == X86_VENDOR_HYGON &&
	       boot_cpu_data.x86 == 0x18;
}

#endif /* _ASM_X86_HYGON_NODE_H */
