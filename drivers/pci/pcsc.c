// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Author: Evangelos Petrongonas <epetron@amazon.de>
 *
 * Implementation of the PCI Configuration Space Cache (PCSC)
 * PCSC is a module which caches the PCI Configuration Space Accesses
 * It implements a write-invalidate policy, meaning that writes are
 * propagated to the device and invalidating the cache. The registers that
 * we are caching are based on the values that are safe to cache and we
 * are not expecting them to change without OS actions.
 *
 */

 #define pr_fmt(fmt) "PCSC: " fmt

#include <linux/atomic.h>
#include <linux/pcsc.h>
#include <linux/sysfs.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

static bool pcsc_enabled;
static int __init pcsc_enabled_setup(char *str)
{
	return kstrtobool(str, &pcsc_enabled) == 0;
}
__setup("pcsc_enabled=", pcsc_enabled_setup);

static bool pcsc_persistence_enabled;
static int __init pcsc_persistence_enabled_setup(char *str)
{
	return kstrtobool(str, &pcsc_persistence_enabled) == 0;
}
__setup("pcsc_persistence_enabled=", pcsc_persistence_enabled_setup);

#define PCSC_KHO_FDT "pcsc"
#define PCSC_KHO_NODE_COMPATIBLE "pcsc-v1"

#ifdef CONFIG_PCSC_STATS
struct pcsc_stats {
	/* Operation Counters */
	unsigned long cache_hits;
	unsigned long cache_misses;
	unsigned long uncachable_reads;
	unsigned long writes;
	unsigned long cache_invalidations;
	unsigned long total_reads;
	unsigned long hw_reads;
	unsigned long device_resets;
	u64 total_cache_access_time; /* in milliseconds */
	u64 total_hw_access_time; /* in milliseconds */
	u64 hw_access_time_due_to_misses; /* in milliseconds */
#ifdef CONFIG_PCSC_KHO
	u64 pcsc_kho_total_restore_time_ns;
	u32 pcsc_kho_restored_device_count;
#endif
};
#endif

static bool pcsc_initialised;
static atomic_t num_nodes = ATOMIC_INIT(0);

#ifdef CONFIG_PCSC_STATS
struct pcsc_stats pcsc_stats;

static inline void pcsc_count_cache_hit(void)
{
	pcsc_stats.cache_hits++;
	pcsc_stats.total_reads++;
}

static inline void pcsc_count_cache_miss(void)
{
	pcsc_stats.cache_misses++;
	pcsc_stats.total_reads++;
	pcsc_stats.hw_reads++;
}

static inline void pcsc_count_uncachable_read(void)
{
	pcsc_stats.uncachable_reads++;
	pcsc_stats.total_reads++;
	pcsc_stats.hw_reads++;
}

static inline void pcsc_count_write(void)
{
	pcsc_stats.writes++;
}

static inline void pcsc_count_cache_invalidation(void)
{
	pcsc_stats.cache_invalidations++;
}

static inline void pcsc_count_device_reset(void)
{
	pcsc_stats.device_resets++;
}
#ifdef CONFIG_PCSC_KHO
static inline void pcsc_count_restored_devices(void)
{
	pcsc_stats.pcsc_kho_restored_device_count++;
}
#endif
#else
static inline void pcsc_count_cache_hit(void)
{
}
static inline void pcsc_count_cache_miss(void)
{
}
static inline void pcsc_count_uncachable_read(void)
{
}
static inline void pcsc_count_write(void)
{
}
static inline void pcsc_count_cache_invalidation(void)
{
}
static inline void pcsc_count_device_reset(void)
{
}
#ifdef CONFIG_PCSC_KHO
static inline void pcsc_count_restored_devices(void)
{
}
#endif
#endif

inline bool pcsc_is_initialised(void)
{
	return pcsc_initialised && pcsc_enabled;
}

static int pcsc_add_bus(struct pci_bus *bus)
{
	if (!bus->orig_ops || !bus->orig_ops->add_bus)
		return 0;
	return bus->orig_ops->add_bus(bus);
}

static void pcsc_remove_bus(struct pci_bus *bus)
{
	if (bus->orig_ops && bus->orig_ops->remove_bus)
		bus->orig_ops->remove_bus(bus);
}

/**
 * pcsc_map_bus - Map PCI configuration space for memory-mapped access
 * @bus: PCI bus structure
 * @devfn: Device and function number
 * @where: Offset in configuration space
 *
 * WARNING: Cache Bypass Issue
 * This function returns a memory-mapped I/O address that provides direct
 * access to PCI configuration space, completely bypassing the PCSC cache.
 *
 * Any reads or writes performed through the returned MMIO address will NOT:
 * - Use cached values for reads
 * - Update cached values on reads
 * - Invalidate cached values on writes
 *
 * This can lead to cache inconsistency where:
 * 1. PCSC cache contains stale data after MMIO writes
 * 2. Subsequent cached reads return outdated values
 * 3. Cache coherency is lost until the next cache invalidation
 *
 * Current users include:
 * - (pci_generic_config_{read,write}{,32}) which are already handled
 * - operations on RCs that are not supported by PCSC.
 * Therefore, there is no risk of cache inconsistency here.
 * However, any future use of map_bus after cache population poses risks.
 *
 * IMPORTANT: Callers using the returned MMIO address are responsible for
 * maintaining cache consistency. Consider invalidating relevant cache entries
 * after MMIO operations if the device's cache may be active.
 *
 * Return: Virtual address for memory-mapped config space access, or NULL
 */
static void __iomem *pcsc_map_bus(struct pci_bus *bus, unsigned int devfn,
				  int where)
{
	if (!bus->orig_ops || !bus->orig_ops->map_bus)
		return NULL;
	return bus->orig_ops->map_bus(bus, devfn, where);
}

/* Weak references to allow architecture-specific overrides */
int __weak pcsc_hw_config_read(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	/*
	 * This function is only called from pcsc_cached_config_read,
	 * which means PCSC ops have already been injected and orig_ops
	 * should be valid.
	 */
	if (bus->orig_ops && bus->orig_ops->read)
		return bus->orig_ops->read(bus, devfn, where, size, val);

	*val = 0xffffffff;
	return PCIBIOS_FUNC_NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(pcsc_hw_config_read);

int __weak pcsc_hw_config_write(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	/*
	 * This function is only called from pcsc_cached_config_write,
	 * which means PCSC ops have already been injected and orig_ops
	 * should be valid.
	 */
	if (bus->orig_ops && bus->orig_ops->write)
		return bus->orig_ops->write(bus, devfn, where, size, val);

	return PCIBIOS_FUNC_NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(pcsc_hw_config_write);

static inline int _test_bits(int where, int size, const void *addr)
{
	int i;
	int res = 1;

	for (i = 0; i < size; i++)
		res &= test_bit(where + i, addr);
	return res;
}

static int pcsc_is_access_cacheable(struct pci_dev *dev, int where, int size)
{
	if (unlikely(!dev || (where + size > PCSC_CFG_SPC_SIZE)))
		return 0;

	return _test_bits(where, size, dev->pcsc->cachable_bitmask);
}

static inline bool pcsc_is_cached(struct pci_dev *dev, int where, int size)
{
	if (unlikely(!dev || !dev->pcsc || !dev->pcsc->cfg_space ||
		     (where + size > PCSC_CFG_SPC_SIZE)))
		return 0;

	return _test_bits(where, size, dev->pcsc->cached_bitmask);
}

static inline void pcsc_set_cached(struct pci_dev *dev, int where, bool cached)
{
	if (WARN_ON(!dev))
		return;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return;

	if (cached)
		set_bit(where, dev->pcsc->cached_bitmask);
	else
		clear_bit(where, dev->pcsc->cached_bitmask);
}

static int pcsc_get_byte(struct pci_dev *dev, int where, u8 *val)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return -EINVAL;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return -EPERM;
	*val = dev->pcsc->cfg_space[where];
	return 0;
}

static int pcsc_update_byte(struct pci_dev *dev, int where, u8 val)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return -EINVAL;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return -EPERM;
	dev->pcsc->cfg_space[where] = val;
	pcsc_set_cached(dev, where, true);

	return 0;
}

static const u8 PCSC_SUPPORTED_CAPABILITIES[] = {
	PCI_CAP_ID_PM,	 PCI_CAP_ID_VPD, PCI_CAP_ID_MSI, PCI_CAP_ID_VNDR,
	PCI_CAP_ID_MSIX, PCI_CAP_ID_EXP, PCI_CAP_ID_AF,	 PCI_CAP_ID_EA
};

#ifdef CONFIG_PCIE_PCSC
static const u16 PCSCS_SUPPORTED_EXT_CAPABILITIES[] = {
	PCI_EXT_CAP_ID_ERR,   PCI_EXT_CAP_ID_ACS, PCI_EXT_CAP_ID_ARI,
	PCI_EXT_CAP_ID_SRIOV, PCI_EXT_CAP_ID_ATS, PCI_EXT_CAP_ID_PRI,
	PCI_EXT_CAP_ID_PASID, PCI_EXT_CAP_ID_DPC, PCI_EXT_CAP_ID_PTM
};

/**
 * pcsc_handle_dpc_cacheability - Set cacheability for DPC capability registers
 * @dev: PCI device
 * @cap_pos: Capability position in config space
 *
 * The DPC capability cacheability depends on whether RP extensions are supported:
 * - PCI_EXP_DPC_CAP_RP_EXT bit indicates RP extension register presence
 */
static void pcsc_handle_dpc_cacheability(struct pci_dev *dev, int cap_pos)
{
	u32 val;
	u16 dpc_cap;
	bool has_rp_extensions;

	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return;

	if (pcsc_hw_config_read(dev->bus, dev->devfn, cap_pos + PCI_EXP_DPC_CAP,
				2, &val) != PCIBIOS_SUCCESSFUL) {
		pci_warn(dev, "PCSC: Failed to read DPC capability at %#x\n",
			 cap_pos + PCI_EXP_DPC_CAP);
		return;
	}

	dpc_cap = val & 0xFFFF;
	has_rp_extensions = !!(dpc_cap & PCI_EXP_DPC_CAP_RP_EXT);

	/* Cache the DPC capability register */
	pcsc_update_byte(dev, cap_pos + PCI_EXP_DPC_CAP, dpc_cap & 0xFF);
	pcsc_update_byte(dev, cap_pos + PCI_EXP_DPC_CAP + 1,
			 (dpc_cap >> 8) & 0xFF);

	/* Always cacheable: main DPC registers */
	bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_EXP_DPC_CAP, 2);
	bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_EXP_DPC_CTL, 2);

	/* Conditionally cacheable: RP extension registers  PCI_EXP_DPC_RP_PIO_MASK
	 * PCI_EXP_DPC_RP_PIO_SEVERITY , PCI_EXP_DPC_RP_PIO_SYSERROR, PCI_EXP_DPC_RP_PIO_EXCEPTION
	 */
	if (has_rp_extensions) {
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_DPC_RP_PIO_MASK, 16);
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_DPC_RP_PIO_SEVERITY, 4);
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_DPC_RP_PIO_SYSERROR, 4);
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_DPC_RP_PIO_EXCEPTION, 4);
	}
}
#endif

/**
 * pcsc_handle_msi_cacheability - Set cacheability for MSI capability registers
 * @dev: PCI device
 * @cap_pos: Capability position in config space
 *
 * The MSI capability has four different shapes (12-24 bytes) depending on:
 * - 64-bit addressing capability (PCI_MSI_FLAGS_64BIT)
 * - Per-vector masking capability (PCI_MSI_FLAGS_MASKBIT)
 *
 * Cacheable registers:
 * - PCI_MSI_FLAGS: Control register
 * - PCI_MSI_ADDRESS_LO: Lower 32 bits of message address
 * - PCI_MSI_ADDRESS_HI: Upper 32 bits (if 64-bit capable)
 * - PCI_MSI_DATA_32/64: Message data register
 * - PCI_MSI_MASK_32/64: Mask bits register (if masking capable)
 *
 * Non-cacheable registers:
 * - PCI_MSI_PENDING_32/64: Pending bits (modified by device)
 */
static void pcsc_handle_msi_cacheability(struct pci_dev *dev, int cap_pos)
{
	u32 val;
	u16 msi_flags;
	bool is_64bit_capable;
	bool is_mask_capable;
	int data_offset;
	int mask_offset;

	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return;

	/* Read MSI flags to determine capability shape */
	if (pcsc_hw_config_read(dev->bus, dev->devfn, cap_pos + PCI_MSI_FLAGS,
				2, &val) != PCIBIOS_SUCCESSFUL) {
		pci_warn(dev, "PCSC: Failed to read MSI flags at %#x\n",
			 cap_pos + PCI_MSI_FLAGS);
		return;
	}

	msi_flags = val & 0xFFFF;
	pcsc_update_byte(dev, cap_pos + PCI_MSI_FLAGS, msi_flags & 0xFF);
	pcsc_update_byte(dev, cap_pos + PCI_MSI_FLAGS + 1, (msi_flags >> 8) & 0xFF);

	/* Mark MSI flags as cacheable */
	bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_MSI_FLAGS, 2);
	is_64bit_capable = !!(msi_flags & PCI_MSI_FLAGS_64BIT);
	is_mask_capable = !!(msi_flags & PCI_MSI_FLAGS_MASKBIT);

	bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_MSI_ADDRESS_LO,
		   4);

	if (is_64bit_capable) {
		/* PCI_MSI_ADDRESS_HI is cacheable for 64-bit capable devices */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_MSI_ADDRESS_HI, 4);

		data_offset = PCI_MSI_DATA_64;
		mask_offset = PCI_MSI_MASK_64;
	} else {
		/* Message Data register is at different offset for 32-bit */
		data_offset = PCI_MSI_DATA_32;
		mask_offset = PCI_MSI_MASK_32;
	}

	/*
	 * Message Data register is always cacheable
	 * Note: PCI spec defines Extended Message Data Capable (bit 9, 0x0200)
	 * which allows 4-byte message data instead of 2-byte. However, Linux
	 * doesn't currently define or use this capability, so we conservatively
	 * mark only 2 bytes as cacheable for compatibility.
	 */
	bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + data_offset, 2);

	if (is_mask_capable) {
		/* Mask bits register is cacheable if masking is supported */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + mask_offset,
			   4);
	}
}

static void infer_capability_cacheability(struct pci_dev *dev, int cap_pos,
					  u8 cap_id)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return;

	switch (cap_id) {
	case PCI_CAP_ID_PM:
		/* Power Management Capability */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_PM_PMC,
			   2); /* PCI_PM_PMC */
		break;
	case PCI_CAP_ID_MSI:
		/* Message Signaled Interrupts */
		pcsc_handle_msi_cacheability(dev, cap_pos);
		break;
	case PCI_CAP_ID_VNDR:
		/* Vendor Specific */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_CAP_FLAGS,
			   1);
		/* Only the flag can be cached as the body is opaque */
		break;
	case PCI_CAP_ID_MSIX:
		/* MSI-X - the entire capability is cacheable */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_MSIX_FLAGS, 10);
		break;
	case PCI_CAP_ID_EXP:
		/* PCI Express capability - All except Status registers */
		bitmap_set(
			dev->pcsc->cachable_bitmask, cap_pos + PCI_EXP_FLAGS,
			8); /* PCI_EXP_FLAGS, PCI_EXP_DEVCAP, PCI_EXP_DEVCTL */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_LNKCAP,
			   6); /* PCI_EXP_LNKCAP, PCI_EXP_LNKCTL */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_SLTCAP,
			   6); /* PCI_EXP_SLTCAP, PCI_EXP_SLTCTL */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_EXP_RTCTL,
			   4); /* PCI_EXP_RTCTL, PCI_EXP_RTCAP */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_DEVCAP2,
			   6); /* PCI_EXP_DEVCAP2, PCI_EXP_DEVCTL2 */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_LNKCAP2,
			   6); /* PCI_EXP_LNKCAP2, PCI_EXP_LNKCTL2 */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EXP_SLTCAP2,
			   6); /* PCI_EXP_SLTCAP2, PCI_EXP_SLTCTL2 */
		break;
	case PCI_CAP_ID_AF:
		/* PCI Advanced Features */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_AF_LENGTH,
			   2); /* PCI_AF_LENGTH, PCI_AF_CAP */
		break;
	case PCI_CAP_ID_EA:
		/* Enhanced Allocation Theoretically the entire capability could
		 * be cached, but it is not trivial to deduce its size.
		 */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_EA_NUM_ENT, 2);
		break;
	case PCI_CAP_ID_VPD:
		/* Vital Product Data */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_VPD_ADDR,
			   2); /* PCI_VPD_ADDR */
		break;
	default:
		/* Unsupported capability - We shouldn't reach this point */
		pr_warn("Something is off when iterating through the supported capabilities.");
		break;
	}
}

static void infer_capabilities_pointers(struct pci_dev *dev)
{
	u8 pos, cap_id, next_cap;
	u32 val;
	int i;

	if (pcsc_hw_config_read(dev->bus, dev->devfn, PCI_CAPABILITY_LIST, 1,
				&val) != PCIBIOS_SUCCESSFUL)
		return;

	pos = (val & 0xFF) & ~0x3;

	while (pos) {
		if (pos < 0x40 || pos > 0xFE)
			break;

		pos &= ~0x3;
		if (pcsc_hw_config_read(dev->bus, dev->devfn, pos, 2, &val) !=
		    PCIBIOS_SUCCESSFUL)
			break;

		cap_id = val & 0xFF; /* PCI_CAP_LIST_ID */
		next_cap = (val >> 8) & 0xFF; /* PCI_CAP_LIST_NEXT */

		bitmap_set(dev->pcsc->cachable_bitmask, pos, 2);
		pcsc_update_byte(dev, pos, cap_id); /* PCI_CAP_LIST_ID */
		pcsc_update_byte(dev, pos + 1,
				 next_cap); /* PCI_CAP_LIST_NEXT */

		pci_dbg(dev, "Capability ID %#x found at %#x\n", cap_id, pos);

		/* Check if this is a supported capability and infer cacheability */
		for (i = 0; i < ARRAY_SIZE(PCSC_SUPPORTED_CAPABILITIES); i++) {
			if (cap_id == PCSC_SUPPORTED_CAPABILITIES[i]) {
				infer_capability_cacheability(dev, pos, cap_id);
				break;
			}
		}

		/* Move to next capability */
		pos = next_cap;
	}
}

#ifdef CONFIG_PCIE_PCSC

static void infer_extended_capability_cacheability(struct pci_dev *dev,
						   int cap_pos, u16 cap_id)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return;

	switch (cap_id) {
	case PCI_EXT_CAP_ID_ERR:
		/* Advanced Error Reporting */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_ERR_UNCOR_MASK,
			   8); /* PCI_ERR_UNCOR_MASK, PCI_ERR_UNCOR_SEVER */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_ERR_COR_MASK,
			   4); /* PCI_ERR_COR_MASK only */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_ERR_ROOT_COMMAND,
			   4); /* PCI_ERR_ROOT_COMMAND */
		break;
	case PCI_EXT_CAP_ID_ACS:
		/* Access Control Services
		 * We only cache PCI_ACS_CAP and PCI_ACS_CTRL (first 4 bytes).
		 * The Egress Control Vector that follows (if present) is not
		 * cached because:
		 * - Determining its size would require reading PCI_ACS_CAP
		 * - These registers are typically only written by the OS during
		 *   setup and not read frequently during runtime
		 * - Caching them would provide no performance benefit
		 */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_ACS_CAP,
			   4); /* PCI_ACS_CAP, PCI_ACS_CTRL */
		break;
	case PCI_EXT_CAP_ID_ARI:
		/* Alternative Routing-ID: */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_ARI_CAP,
			   4); /* PCI_ARI_CAP, PCI_ARI_CTRL */
		break;
	case PCI_EXT_CAP_ID_SRIOV:
		/* SR-IOV */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_SRIOV_CAP,
			   6); /* PCI_SRIOV_CAP, PCI_SRIOV_CTRL */
		/* PCI_SRIOV_INITIAL_VF, PCI_SRIOV_TOTAL_VF,
		 * PCI_SRIOV_NUM_VF,PCI_SRIOV_FUNC_LINK
		 */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_SRIOV_INITIAL_VF, 7);
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_SRIOV_VF_OFFSET,
			   4); /* PCI_SRIOV_VF_OFFSET, PCI_SRIOV_VF_STRIDE */
		/* PCI_SRIOV_VF_DID, PCI_SRIOV_SUPPORTED_PAGE_SIZES,PCI_SRIOV_PAGE_SIZE */
		bitmap_set(
			dev->pcsc->cachable_bitmask, cap_pos + PCI_SRIOV_VF_DID,
			10);
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_SRIOV_BAR,
			   24); /* PCI_SRIOV_BAR0-5 */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_SRIOV_VFM,
			   4); /* PCI_SRIOV_VFMM */
		break;
	case PCI_EXT_CAP_ID_ATS:
		/* Address Translation Service: */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_ATS_CAP,
			   4); /* PCI_ATS_CAP, PCI_ATS_CTRL*/
		break;
	case PCI_EXT_CAP_ID_PRI:
		/* Page Request Interface */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_PRI_CTRL,
			   2); /* PCI_PRI_CTRL */
		bitmap_set(dev->pcsc->cachable_bitmask,
			   cap_pos + PCI_PRI_MAX_REQ,
			   8); /* PCI_PRI_MAX_REQ, PCI_PRI_ALLOC_REQ */
		break;
	case PCI_EXT_CAP_ID_PASID:
		/* Process Address Space ID */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_PASID_CAP,
			   4); /* PCI_PASID_CAP, PCI_PASID_CTRL */
		break;
	case PCI_EXT_CAP_ID_DPC:
		/* Downstream Port Containment */
		pcsc_handle_dpc_cacheability(dev, cap_pos);
		break;
	case PCI_EXT_CAP_ID_PTM:
		/* Precision Time Measurement */
		bitmap_set(dev->pcsc->cachable_bitmask, cap_pos + PCI_PTM_CAP,
			   8); /* PCI_PTM_CAP, PCI_PTM_CTRL */
		break;
	default:
		/* Unknown extended capability - only cache header */
		break;
	}
}

static void infer_extended_capabilities_pointers(struct pci_dev *dev)
{
	int pos = 0x100;
	u32 header;
	int cap_ver, cap_id;
	int i;

	while (pos) {
		if (pos > 0xFFC || pos < 0x100)
			break;

		pos &= ~0x3;

		if (pcsc_hw_config_read(dev->bus, dev->devfn, pos, 4,
					&header) != PCIBIOS_SUCCESSFUL)
			break;

		if (!header)
			break;

		bitmap_set(dev->pcsc->cachable_bitmask, pos, 4);
		for (i = 0; i < 4; i++)
			pcsc_update_byte(dev, pos + i,
					 (header >> (i * 8)) & 0xFF);

		cap_id = PCI_EXT_CAP_ID(header);
		cap_ver = PCI_EXT_CAP_VER(header);

		pci_dbg(dev,
			"Extended capability ID %#x (ver %d) found at %#x, next cap at %#x\n",
			cap_id, cap_ver, pos, PCI_EXT_CAP_NEXT(header));

		/* Check if this is a supported extended capability and infer cacheability */
		for (i = 0; i < ARRAY_SIZE(PCSCS_SUPPORTED_EXT_CAPABILITIES);
		     i++) {
			if (cap_id == PCSCS_SUPPORTED_EXT_CAPABILITIES[i]) {
				infer_extended_capability_cacheability(dev, pos,
								       cap_id);
				break;
			}
		}

		pos = PCI_EXT_CAP_NEXT(header);
	}
}
#endif

static void infer_cacheability(struct pci_dev *dev)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return;

	bitmap_zero(dev->pcsc->cachable_bitmask, PCSC_CFG_SPC_SIZE);

	/* Type 0 Configuration Space Header */
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		/*
		 * Mark cacheable registers in the PCI configuration space header.
		 * We cache read-only and rarely changing registers:
		 * - PCI_VENDOR_ID, PCI_DEVICE_ID (0x00-0x03)
		 * - PCI_CLASS_REVISION through PCI_CAPABILITY_LIST (0x08-0x34)
		 *   Includes: CLASS_REVISION, CACHE_LINE_SIZE, LATENCY_TIMER,
		 *   HEADER_TYPE, BIST, BASE_ADDRESS_0-5, CARDBUS_CIS,
		 *   SUBSYSTEM_VENDOR_ID, SUBSYSTEM_ID, ROM_ADDRESS, CAPABILITY_LIST
		 * - PCI_INTERRUPT_LINE through PCI_MAX_LAT (0x3c-0x3f)
		 *   Includes: INTERRUPT_LINE, INTERRUPT_PIN, MIN_GNT, MAX_LAT
		 */
		bitmap_set(dev->pcsc->cachable_bitmask, PCI_VENDOR_ID, 4);
		bitmap_set(dev->pcsc->cachable_bitmask, PCI_CLASS_REVISION, 45);
		bitmap_set(dev->pcsc->cachable_bitmask, PCI_INTERRUPT_LINE, 4);

		/* Pre populate the cache with the values that we already know */
		pcsc_update_byte(dev, PCI_HEADER_TYPE,
				 dev->hdr_type |
					 (dev->multifunction ? 0x80 : 0));

		/*
		 * SR-IOV VFs must return 0xFFFF (PCI_ANY_ID) for vendor/device ID
		 * registers per PCIe spec.
		 */
		if (dev->is_virtfn) {
			pcsc_update_byte(dev, PCI_VENDOR_ID, 0xFF);
			pcsc_update_byte(dev, PCI_VENDOR_ID + 1, 0xFF);
			pcsc_update_byte(dev, PCI_DEVICE_ID, 0xFF);
			pcsc_update_byte(dev, PCI_DEVICE_ID + 1, 0xFF);
		} else {
			if (dev->vendor != PCI_ANY_ID) {
				pcsc_update_byte(dev, PCI_VENDOR_ID,
						 dev->vendor & 0xFF);
				pcsc_update_byte(dev, PCI_VENDOR_ID + 1,
						 (dev->vendor >> 8) & 0xFF);
			}
			if (dev->device != PCI_ANY_ID) {
				pcsc_update_byte(dev, PCI_DEVICE_ID,
						 dev->device & 0xFF);
				pcsc_update_byte(dev, PCI_DEVICE_ID + 1,
						 (dev->device >> 8) & 0xFF);
			}
		}

		infer_capabilities_pointers(dev);
#ifdef CONFIG_PCIE_PCSC
		if (pci_is_pcie(dev))
			infer_extended_capabilities_pointers(dev);
#endif
	}
}

#ifdef CONFIG_PCSC_KHO
static struct page *pcsc_kho_fdt;
static int pcsc_kho_fdt_order;

static int pcsc_kho_save_device(struct pci_dev *dev, void *fdt)
{
	char node_name[32];
	size_t data_size, total_size;
	u64 data_addr;
	int err = 0;

	if (!dev->pcsc || !dev->pcsc->data)
		return 1;

	if (dev->hdr_type != PCI_HEADER_TYPE_NORMAL)
		return 1;

	/* Create FDT node for this device - node name contains device identifer */
	snprintf(node_name, sizeof(node_name), "dev_%04x_%02x_%02x_%x",
		 pci_domain_nr(dev->bus), dev->bus->number,
		 PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));

	err = fdt_begin_node(fdt, node_name);
	if (err) {
		pci_err(dev, "PCSC: Failed to begin FDT node '%s': %d\n",
			node_name, err);
		return err;
	}

	data_size = sizeof(struct pcsc_data);
	total_size = PAGE_ALIGN(data_size);

	data_addr = virt_to_phys(dev->pcsc->data);
	err = kho_preserve_phys(data_addr, total_size);
	if (err) {
		pci_err(dev, "PCSC: Failed to preserve data buffer: %d\n", err);
		return err;
	}

	err = fdt_property(fdt, "da", &data_addr, sizeof(data_addr));
	if (err) {
		pci_err(dev, "PCSC: Failed to set da property: %d\n",
			err);
		return err;
	}

	err = fdt_end_node(fdt);
	if (err) {
		pci_err(dev, "PCSC: Failed to end FDT node: %d\n", err);
		return err;
	}

	return 0;
}

static int pcsc_kho_notifier(struct notifier_block *self, unsigned long cmd,
			     void *v)
{
	struct kho_serialization *ser = v;
	struct pci_dev *dev = NULL;
	void *fdt;
	int err = 0;
	size_t fdt_size;
	u32 dev_count = 0;
	u32 eligible_count = 0;
	u32 saved_count = 0;
	u32 skipped_count = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		if (pcsc_kho_fdt) {
			__free_pages(pcsc_kho_fdt, pcsc_kho_fdt_order);
			pcsc_kho_fdt = NULL;
		}
		return NOTIFY_DONE;
	case KEXEC_KHO_FINALIZE:
		/* Handled below */
		break;
	default:
		return NOTIFY_BAD;
	}

#ifdef CONFIG_PCSC_STATS
	ktime_t start_time = ktime_get();
#endif

	for_each_pci_dev(dev) {
		dev_count++;
		if (dev->pcsc && dev->pcsc->cfg_space &&
		    dev->hdr_type == PCI_HEADER_TYPE_NORMAL)
			eligible_count++;
	}

	pr_info("Total PCI devices: %u, eligible for save: %u\n",
		dev_count, eligible_count);

	if (eligible_count == 0)
		return NOTIFY_DONE;

	/* Allocate FDT with size calculation (conservative estimates):
	 * - Per device: node_name(~20) + node_overhead(~12) + da_property(~20)
	 *   = ~52 bytes, round up to 64 for alignment/margin
	 * - Fixed overhead: header(40) + root_node(~40) + strings_table(~30)
	 * + misc(~32) = ~144 bytes, round up to 256
	 */
	fdt_size = PAGE_ALIGN((eligible_count * 64 + 256));
	pcsc_kho_fdt_order = get_order(fdt_size);
	pcsc_kho_fdt = alloc_pages(GFP_KERNEL, pcsc_kho_fdt_order);
	if (!pcsc_kho_fdt) {
		pr_err("PCSC: Failed to allocate FDT pages (size=%zu, order=%d)\n",
		       fdt_size, pcsc_kho_fdt_order);
		return NOTIFY_BAD;
	}

	fdt = page_to_virt(pcsc_kho_fdt);

	/* Create FDT */
	err = fdt_create(fdt, fdt_size);
	if (err) {
		pr_err("PCSC: Failed to create FDT: %d\n", err);
		goto error_cleanup;
	}

	err = fdt_finish_reservemap(fdt);
	if (err) {
		pr_err("PCSC: Failed to finish FDT reservemap: %d\n", err);
		goto error_cleanup;
	}

	err = fdt_begin_node(fdt, "");
	if (err) {
		pr_err("PCSC: Failed to begin root FDT node: %d\n", err);
		goto error_cleanup;
	}

	err = fdt_property_string(fdt, "compatible", PCSC_KHO_NODE_COMPATIBLE);
	if (err) {
		pr_err("PCSC: Failed to set compatible property: %d\n", err);
		goto error_cleanup;
	}

	for_each_pci_dev(dev) {
		int save_err = pcsc_kho_save_device(dev, fdt);

		if (save_err == 0) {
			saved_count++;
		} else if (save_err == 1) {
			/* Skipped (not eligible) */
			skipped_count++;
		} else {
			pr_err("Failed to save device %04x:%02x:%02x.%d: %d\n",
			       pci_domain_nr(dev->bus), dev->bus->number,
			       PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn),
			       save_err);
			break;
		}
	}

	err = fdt_end_node(fdt);
	if (err) {
		pr_err("Failed to end root FDT node: %d\n", err);
		goto error_cleanup;
	}

	err = fdt_finish(fdt);
	if (err) {
		pr_err("Failed to finish FDT: %d\n", err);
		goto error_cleanup;
	}

	int fdt_final_size = fdt_totalsize(fdt);
	int num_pages = PAGE_ALIGN(fdt_final_size) / PAGE_SIZE;

	err = kho_preserve_phys(page_to_phys(pcsc_kho_fdt),
				num_pages * PAGE_SIZE);
	if (err) {
		pr_err("Failed to preserve FDT pages: %d\n", err);
		goto error_cleanup;
	}

	err = kho_add_subtree(ser, PCSC_KHO_FDT, fdt);
	if (err) {
		pr_err("Failed to add FDT to KHO tree: %d\n", err);
		goto error_cleanup;
	}

#ifdef CONFIG_PCSC_STATS
	ktime_t end_time = ktime_get();
	u64 duration_ns = ktime_to_ns(ktime_sub(end_time, start_time));
	u64 duration_us = duration_ns / 1000;

	pr_info("Saved %u devices to KHO in %llu us (%llu.%03llu ms)\n",
		saved_count, duration_us, duration_us / 1000,
		duration_us % 1000);
#endif
	return NOTIFY_DONE;

error_cleanup:
	pr_err("KHO save failed with error %d\n", err);
	__free_pages(pcsc_kho_fdt, pcsc_kho_fdt_order);
	pcsc_kho_fdt = NULL;
	return NOTIFY_BAD;
}

static struct notifier_block pcsc_kho_nb = {
	.notifier_call = pcsc_kho_notifier,
};

static bool pcsc_kho_restore_device(struct pci_dev *dev, const void *fdt,
				    int node)
{
	const struct pcsc_data *preserved_data;
	const u64 *data_addr;
	int len;

	data_addr = fdt_getprop(fdt, node, "da", &len);
	if (!data_addr || len != sizeof(*data_addr))
		return false;

	preserved_data = phys_to_virt(*data_addr);
	if (!preserved_data)
		return false;


	dev->pcsc->data = (struct pcsc_data *)preserved_data;
	dev->pcsc->cachable_bitmask = dev->pcsc->data->cachable_bitmask;
	dev->pcsc->cached_bitmask = dev->pcsc->data->cached_bitmask;
	dev->pcsc->cfg_space = dev->pcsc->data->cfg_space;

	return true;
}

static bool pcsc_kho_check_restore(struct pci_dev *dev)
{
	phys_addr_t fdt_phys;
	const void *fdt;
	int node, err;
	bool restored = false;
	char node_name[32];
#ifdef CONFIG_PCSC_STATS
	ktime_t start_time, end_time;
	u64 duration_ns;
#endif

	err = kho_retrieve_subtree(PCSC_KHO_FDT, &fdt_phys);
	if (err) {
		pci_dbg(dev, "PCSC: kho_retrieve_subtree failed: %d\n", err);
		return false;
	}

	fdt = phys_to_virt(fdt_phys);
	if (fdt_node_check_compatible(fdt, 0, PCSC_KHO_NODE_COMPATIBLE)) {
		pci_dbg(dev, "PCSC: FDT node not compatible\n");
		return false;
	}

#ifdef CONFIG_PCSC_STATS
	start_time = ktime_get();
#endif

	snprintf(node_name, sizeof(node_name), "dev_%04x_%02x_%02x_%x",
		 pci_domain_nr(dev->bus), dev->bus->number,
		 PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));

	node = fdt_subnode_offset(fdt, 0, node_name);
	if (node >= 0)
		restored = pcsc_kho_restore_device(dev, fdt, node);

#ifdef CONFIG_PCSC_STATS
	if (restored) {
		end_time = ktime_get();
		duration_ns = ktime_to_ns(ktime_sub(end_time, start_time));

		pcsc_stats.pcsc_kho_total_restore_time_ns += duration_ns;
		pcsc_count_restored_devices();
	}
#endif

	return restored;
}
#endif

int pcsc_add_device(struct pci_dev *dev)
{
	struct pcsc_node *node;
	struct pci_bus *bus;
	size_t data_size;

	if (WARN_ON(!dev))
		return -EINVAL;

	bus = dev->bus;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	dev->pcsc = node;
	/* The current version of the PCSC supports only endpoint devices.
	 * Bridges and RCs are not supported, but we are still creating
	 * nodes for these devices, as it simplifies the code flow
	 */
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
#ifdef CONFIG_PCSC_KHO
		bool restored = false;

		/* Try to restore from KHO first, before any allocation */
		if (pcsc_persistence_enabled && kho_is_enabled())
			restored = pcsc_kho_check_restore(dev);

		if (!restored) {
#endif
			/* Allocate contiguous, page aligned data block. This is
			 * needed for persisting the data with KHO.
			 */
			data_size = sizeof(struct pcsc_data);

			dev->pcsc->data =
				(struct pcsc_data *)__get_free_pages(
					GFP_KERNEL | __GFP_ZERO, get_order(data_size));
			if (!dev->pcsc->data)
				goto err_free_node;

			dev->pcsc->cachable_bitmask = dev->pcsc->data->cachable_bitmask;
			dev->pcsc->cached_bitmask = dev->pcsc->data->cached_bitmask;
			dev->pcsc->cfg_space = dev->pcsc->data->cfg_space;

			infer_cacheability(dev);
#ifdef CONFIG_PCSC_KHO
		}
#endif
	} else {
		dev->pcsc->data = NULL;
		dev->pcsc->cachable_bitmask = NULL;
		dev->pcsc->cached_bitmask = NULL;
		dev->pcsc->cfg_space = NULL;
	}

	atomic_inc(&num_nodes);
	pci_dbg(dev, "PCSC: Created cache node\n");

	return 0;

err_free_node:
	dev->pcsc = NULL;
	kfree(node);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(pcsc_add_device);

int pcsc_remove_device(struct pci_dev *dev)
{
	if (WARN_ON(!dev))
		return -EINVAL;

	pci_dbg(dev, "PCSC: Removing cache node");

	atomic_dec(&num_nodes);

	if (dev->pcsc && dev->pcsc->data) {
		size_t data_size = sizeof(struct pcsc_data);
		size_t total_size = PAGE_ALIGN(data_size);

		free_pages((unsigned long)dev->pcsc->data,
			get_order(total_size));
		kfree(dev->pcsc);
	}
	dev->pcsc = NULL;

	return 0;
}
EXPORT_SYMBOL_GPL(pcsc_remove_device);

/**
 * pcsc_get_and_insert_multiple - Read multiple bytes from PCI cache or HW
 * @dev: PCI device to read from
 * @bus: PCI bus to read from
 * @devfn: device and function number
 * @where: offset in config space
 * @word: pointer to store read value
 * @size: number of bytes to read (1, 2 or 4)
 *
 * Reads consecutive bytes from PCI cache or hardware. If values are not cached,
 * reads from hardware and inserts into cache.
 *
 * Return: 0 on success, negative error code on failure
 */
static int pcsc_get_and_insert_multiple(struct pci_dev *dev,
					struct pci_bus *bus, unsigned int devfn,
					int where, u32 *word, int size)
{
	u32 word_cached = 0;
	u8 byte_val;
	int rc, i;
#ifdef CONFIG_PCSC_STATS
	ktime_t start_time;
	u64 duration;
#endif

	if (WARN_ON(!dev || !bus || !word))
		return -EINVAL;

	if (WARN_ON(size != 1 && size != 2 && size != 4))
		return -EINVAL;

	if (where + size > PCSC_CFG_SPC_SIZE)
		return -EINVAL;

	if (pcsc_is_cached(dev, where, size)) {
		/* Read bytes from cache and assemble them into word_cached
		 * in little-endian order (as per PCI spec)
		 */
		for (i = 0; i < size; i++) {
			pcsc_get_byte(dev, where + i, &byte_val);
			word_cached |= ((u32)byte_val << (i * 8));
		}
		pcsc_count_cache_hit();
	} else {
#ifdef CONFIG_PCSC_STATS
		start_time = ktime_get();
#endif
		rc = pcsc_hw_config_read(bus, devfn, where, size, &word_cached);
#ifdef CONFIG_PCSC_STATS
		duration = ktime_to_ns(ktime_sub(ktime_get(), start_time));
		pcsc_stats.hw_access_time_due_to_misses += duration;
		pcsc_stats.total_hw_access_time += duration;
#endif
		if (rc) {
			pci_err(dev,
				"%s: Failed to read CFG Space where=%d size=%d",
				__func__, where, size);
			return rc;
		}

		/* Extract bytes from word_cached in little-endian order
		 * and store them in cache.
		 */
		for (i = 0; i < size; i++) {
			byte_val = (word_cached >> (i * 8)) & 0xFF;
			pcsc_update_byte(dev, where + i, byte_val);
		}
		pcsc_count_cache_miss();
	}

	*word = word_cached;
	return 0;
}

int pcsc_cached_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val)
{
	int rc;
	struct pci_dev *dev;
#ifdef CONFIG_PCSC_STATS
	ktime_t hw_start_time;
	u64 hw_duration;
#endif

#ifdef CONFIG_PCSC_STATS
	u64 duration;
	ktime_t start_time;

	start_time = ktime_get();
#endif

	if (unlikely(!pcsc_is_initialised()))
		goto read_from_dev;

	if (WARN_ON(!bus || !val || (size != 1 && size != 2 && size != 4) ||
		    where + size > PCSC_CFG_SPC_SIZE))
		return -EINVAL;

	dev = pci_get_slot(bus, devfn);

	if (unlikely(!dev || !dev->pcsc))
		goto read_from_dev;

	if (dev->pcsc->cfg_space &&
	    pcsc_is_access_cacheable(dev, where, size)) {
		rc = pcsc_get_and_insert_multiple(dev, bus, devfn, where, val,
						  size);
#ifdef CONFIG_PCSC_STATS
		duration = ktime_to_ns(ktime_sub(ktime_get(), start_time));
		pcsc_stats.total_cache_access_time += duration;
#endif
		if (likely(!rc)) {
			pci_dev_put(dev);
			return 0;
		}
		/* if reading from the cache failed continue and try reading
		 * from the actual device
		 */
	} else {
		if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL)
			pcsc_count_uncachable_read();
	}
read_from_dev:
#ifdef CONFIG_PCSC_STATS
	hw_start_time = ktime_get();
#endif
	if (dev)
		pci_dev_put(dev);
	rc = pcsc_hw_config_read(bus, devfn, where, size, val);
#ifdef CONFIG_PCSC_STATS
	hw_duration = ktime_to_ns(ktime_sub(ktime_get(), hw_start_time));
	/* Add timing for uncacheable reads */
	pcsc_stats.total_hw_access_time += hw_duration;
#endif
	return rc;
}
EXPORT_SYMBOL_GPL(pcsc_cached_config_read);

int pcsc_cached_config_write(struct pci_bus *bus, unsigned int devfn, int where,
			     int size, u32 val)
{
	int i;
	struct pci_dev *dev;
	int rc;
#ifdef CONFIG_PCSC_STATS
	ktime_t hw_start_time;
	u64 hw_duration;
#endif

	if (unlikely(!pcsc_is_initialised()))
		goto write_to_dev;

	if (WARN_ON(!bus || (size != 1 && size != 2 && size != 4) ||
		    where + size > PCSC_CFG_SPC_SIZE))
		return -EINVAL;

	dev = pci_get_slot(bus, devfn);

	if (unlikely(!dev || !dev->pcsc || !dev->pcsc->cfg_space)) {
		/* Do not add nodes on arbitrary writes  */
		goto write_to_dev;
	} else {
		/* Mark the cache as dirty */
		if (pcsc_is_access_cacheable(dev, where, size)) {
			for (i = 0; i < size; i++)
				pcsc_set_cached(dev, where + i, false);
			pcsc_count_cache_invalidation();
		}
	}
write_to_dev:
	pcsc_count_write();
	if (dev)
		pci_dev_put(dev);
#ifdef CONFIG_PCSC_STATS
	hw_start_time = ktime_get();
#endif
	rc = pcsc_hw_config_write(bus, devfn, where, size, val);
#ifdef CONFIG_PCSC_STATS
	hw_duration = ktime_to_ns(ktime_sub(ktime_get(), hw_start_time));
	pcsc_stats.total_hw_access_time += hw_duration;
#endif
	return rc;
}
EXPORT_SYMBOL_GPL(pcsc_cached_config_write);

int pcsc_device_reset(struct pci_dev *dev)
{
	if (unlikely((!dev)))
		return -EINVAL;

	if (unlikely(!pcsc_is_initialised()))
		return 0;

	/* The layout of the CFG Space is not going to change after a device
	 * reset, whether the reset is FLR or conventional. Only the values
	 * are going to change. We could further optimise the cache to maintain
	 * some of the HWInt values that are going to remain constant after a reset.
	 */
	bitmap_zero(dev->pcsc->cached_bitmask, PCSC_CFG_SPC_SIZE);
	pcsc_count_device_reset();
	return 0;
}

static struct pci_ops pcsc_ops = {
	.add_bus = pcsc_add_bus,
	.remove_bus = pcsc_remove_bus,
	.map_bus = pcsc_map_bus,
	.read = pcsc_cached_config_read,
	.write = pcsc_cached_config_write,
};

int pcsc_inject_bus_ops(struct pci_bus *bus)
{
	if (!bus)
		return -EINVAL;

	if (!bus->ops) {
		WARN_ONCE(
			1,
			"PCSC: Cannot inject ops - bus %04x:%02x ops not defined\n",
			pci_domain_nr(bus), bus->number);
		return -EINVAL;
	}

	if (bus->ops->read == pcsc_cached_config_read || bus->orig_ops)
		return 0;

	bus->orig_ops = bus->ops;
	bus->ops = &pcsc_ops;

	pci_dbg(bus, "PCSC: Injected ops for bus");
	return 0;
}
EXPORT_SYMBOL_GPL(pcsc_inject_bus_ops);

static void pcsc_remove_bus_ops(struct pci_bus *bus)
{
	if (bus->orig_ops && bus->ops == &pcsc_ops) {
		bus->ops = bus->orig_ops;
		bus->orig_ops = NULL;
	}
}

static int pcsc_bus_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct device *dev = data;
	struct pci_bus *bus;

	bus = to_pci_bus(dev);
	if (!bus)
		return NOTIFY_OK;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		pcsc_inject_bus_ops(bus);
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		/*
		 * Remove on DEL_DEVICE to unhook before device_del() completes.
		 * This ensures caching is disabled before the final cleanup.
		 */
		pcsc_remove_bus_ops(bus);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block pcsc_bus_nb = {
	.notifier_call = pcsc_bus_notify,
};

static ssize_t pcsc_enabled_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", pcsc_enabled);
}

static ssize_t pcsc_enabled_store(struct kobject *kobj,
				  struct kobj_attribute *attr, const char *buf,
				  size_t count)
{
	bool new_value;
	int ret;

	ret = kstrtobool(buf, &new_value);
	if (ret < 0)
		return ret;

	pcsc_enabled = new_value;
	return count;
}

static struct kobj_attribute pcsc_enabled_attribute =
	__ATTR(enabled, 0644, pcsc_enabled_show, pcsc_enabled_store);

#ifdef CONFIG_PCSC_STATS
static ssize_t pcsc_stats_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;

	ret = sysfs_emit(
		buf,
		"Cache Hits: %lu\n"
		"Cache Misses: %lu\n"
		"Uncachable Reads: %lu\n"
		"Writes: %lu\n"
		"Cache Invalidations: %lu\n"
		"Device Resets: %lu\n"
		"Total Reads: %lu\n"
		"Hardware Reads: %lu\n"
		"Hit Rate: %lu%%\n"
		"Total Cache Access Time: %llu us\n"
		"Cache Access Time (without HW reads due to Misses): %llu us\n"
		"HW Access Time due to misses: %llu us\n"
		"Total Hardware Access Time: %llu us\n",
		pcsc_stats.cache_hits, pcsc_stats.cache_misses,
		pcsc_stats.uncachable_reads, pcsc_stats.writes,
		pcsc_stats.cache_invalidations, pcsc_stats.device_resets,
		pcsc_stats.total_reads,
		pcsc_stats.hw_reads,
		pcsc_stats.total_reads ?
			      (pcsc_stats.cache_hits * 100) / pcsc_stats.total_reads :
			      0,
		pcsc_stats.total_cache_access_time / 1000,
		(pcsc_stats.total_cache_access_time -
		 pcsc_stats.hw_access_time_due_to_misses) /
			1000,
		pcsc_stats.hw_access_time_due_to_misses / 1000,
		pcsc_stats.total_hw_access_time / 1000);

#ifdef CONFIG_PCSC_KHO
	u64 total_restore_time_us = pcsc_stats.pcsc_kho_total_restore_time_ns / 1000;

	ret += sysfs_emit_at(buf, ret,
			     "KHO Restore Statistics:\n"
			     "  Restored Devices: %u\n"
			     "  Total Restore Time: %llu us\n",
			     pcsc_stats.pcsc_kho_restored_device_count,
			     total_restore_time_us);

#endif

	return ret;
}

static struct kobj_attribute pcsc_stats_attribute =
	__ATTR(stats, 0444, pcsc_stats_show, NULL);
#endif

static struct attribute *pcsc_attrs[] = {
	&pcsc_enabled_attribute.attr,
#ifdef CONFIG_PCSC_STATS
	&pcsc_stats_attribute.attr,
#endif
	NULL,
};

static struct attribute_group pcsc_attr_group = {
	.attrs = pcsc_attrs,
};

static struct kobject *pcsc_kobj;

static void pcsc_create_sysfs(void)
{
	struct kset *pci_bus_kset;
	int ret;

	if (pcsc_kobj)
		return; /* Already created */

	pci_bus_kset = bus_get_kset(&pci_bus_type);
	if (!pci_bus_kset) {
		/* PCI bus kset not ready yet, will be retried later */
		return;
	}

	pcsc_kobj = kobject_create_and_add("pcsc", &pci_bus_kset->kobj);
	if (!pcsc_kobj) {
		pr_err("Failed to create sysfs kobject\n");
		return;
	}

	ret = sysfs_create_group(pcsc_kobj, &pcsc_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs group\n");
		kobject_put(pcsc_kobj);
		pcsc_kobj = NULL;
		return;
	}
}

static int __init pcsc_init(void)
{
#ifdef CONFIG_PCSC_KHO
	int ret;
#endif

	bus_register_notifier(&pci_bus_type, &pcsc_bus_nb);

	/* Try to create sysfs entry, but don't fail if PCI bus isn't ready yet */
	pcsc_create_sysfs();

#ifdef CONFIG_PCSC_STATS
	memset(&pcsc_stats, 0, sizeof(pcsc_stats));
#endif

#ifdef CONFIG_PCSC_KHO
	/* Register KHO notifier if persistence is enabled */
	if (pcsc_persistence_enabled && kho_is_enabled()) {
		ret = register_kho_notifier(&pcsc_kho_nb);
		if (ret == 0)
			pr_info("KHO notifier registered successfully\n");
		else
			pr_err("Failed to register KHO notifier: %d\n", ret);
	}
#endif /* CONFIG_PCSC_KHO */

	pcsc_initialised = true;
	pr_info("initialised (enabled=%d, persistence=%d)\n",
		pcsc_enabled, pcsc_persistence_enabled);

	return 0;
}

/* Late initcall to retry sysfs creation if it failed during core_initcall */
static int __init pcsc_sysfs_init(void)
{
	pcsc_create_sysfs();
	return 0;
}

core_initcall(pcsc_init);

/*
 * The PCI subsystem is initialised later, therefore we need to add
 * our sysfs entries later. This is done to avoid modifying the sysfs
 * creation of the core pci driver.
 */
late_initcall(pcsc_sysfs_init);
