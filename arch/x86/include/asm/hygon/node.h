/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hygon Family 0x18 Data Fabric node enumeration API
 *
 * This header declares Hygon Fam18h DF node enumeration and function
 * access interfaces.
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

/*
 * Hygon Core and DDR Dies (CDDs) start at DFID 4. Each CDD contains
 * CPU cores and UMCs. I/O Dies (IODs) occupy DFIDs 0-3.
 */
#define HYGON_CDD_DFID_BASE	4

/**
 * enum hygon_node_type - DF node type, derived from DFID
 * @HYGON_NODE_IOD: I/O die (DFID < HYGON_CDD_DFID_BASE); no UMC.
 * @HYGON_NODE_CDD: Core and DDR Die (DFID >= HYGON_CDD_DFID_BASE);
 *                  contains CPU cores and UMC controllers.
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
 * This is the upper bound for every node index in this API.
 *
 * Return: total node count, or 0 if the cache is not ready or this is
 * not a Hygon Fam18h platform.
 */
u16 hygon_node_num(void);

/**
 * hygon_cdd_num() - number of Core and DDR Dies (CDD)
 *
 * A CDD (DFID >= 4) has CPU cores and UMC controllers; an IOD
 * (DFID < 4) has neither. CDDs occupy nodes [0, hygon_cdd_num()).
 *
 * Return: CDD count, or 0 if the cache is not ready.
 */
u16 hygon_cdd_num(void);

/**
 * hygon_node_get_info() - read identity snapshot for a DF node
 * @node: DF node index in [0, hygon_node_num())
 * @info: output structure (socket_id, dfid, type)
 *
 * The identity fields are fixed after enumeration and are returned
 * together in one lookup.
 *
 * Return: 0 on success, -EINVAL if @node is out of range or @info is
 * NULL, -ENODEV if the cache is not ready.
 */
int hygon_node_get_info(u16 node, struct hygon_node_info *info);

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
 * domain, bus and slot as @pdev, for drivers that hold a sibling
 * function and need the containing node index.
 *
 * Return: DF node index in [0, hygon_node_num()) on success, -EINVAL
 * if @pdev is NULL, or -ENODEV if no matching node is found or the
 * cache is not ready.
 */
int hygon_pci_dev_to_df_node(struct pci_dev *pdev);

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

static inline struct pci_dev *hygon_node_get_func(u16 node, u8 func)
{
	return NULL;
}

static inline int hygon_pci_dev_to_df_node(struct pci_dev *pdev)
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
