/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCI Crash Buffer - Capture PCI config space at panic time
 *
 * This module captures PCI configuration space data (including AER
 * extended capability registers) for all PCI devices at panic time.
 * The data is stored in a buffer whose pages are captured in the
 * vmcore for off-site analysis.
 *
 * Copyright (c) 2026 Amazon.com, Inc. or its affiliates.
 */
#ifndef _LINUX_PCI_CRASH_H
#define _LINUX_PCI_CRASH_H

#include <linux/types.h>

#define PCI_CRASH_MAGIC         0x50434943  /* "PCIC" in ASCII */
#define PCI_CRASH_VERSION       1

/**
 * struct pci_crash_buffer_header - Header for PCI crash buffer
 * @magic:        Magic number (PCI_CRASH_MAGIC)
 * @version:      Format version (PCI_CRASH_VERSION)
 * @device_count: Number of device records following this header
 * @config_size:  0 -- indicates variable-length records. Each device
 *                record stores its own config_size (pdev->cfg_size:
 *                256 for legacy PCI, 4096 for PCIe). Parsers walk
 *                records sequentially using per-record config_size.
 * @timestamp:    Capture timestamp from ktime_get_real_fast_ns()
 * @flags:        Reserved for future use (0 for now)
 * @reserved:     Padding to align to 32 bytes
 *
 * Total size: 32 bytes
 */
struct pci_crash_buffer_header {
	__le32 magic;
	__le32 version;
	__le32 device_count;
	__le32 config_size;
	__le64 timestamp;
	__le32 flags;
	__le32 reserved;
} __packed;

/**
 * struct pci_crash_device_record - Per-device record in crash buffer
 * @domain:      PCI domain number
 * @bus:         PCI bus number
 * @devfn:       Device and function number (PCI_DEVFN format)
 * @config_size: Config space size for this device (pdev->cfg_size:
 *               256 for legacy PCI, 4096 for PCIe)
 * @config_data: Raw PCI config space (config_size bytes)
 *
 * Records are variable-length: total size per record is
 * PCI_CRASH_RECORD_META + config_size bytes.
 */
struct pci_crash_device_record {
	__le16 domain;
	__u8   bus;
	__u8   devfn;
	__le32 config_size;
	__u8   config_data[];
} __packed;

#define PCI_CRASH_HEADER_SIZE  sizeof(struct pci_crash_buffer_header)
#define PCI_CRASH_RECORD_META  sizeof(struct pci_crash_device_record)

/**
 * struct pci_crash_pagemap - Physical page directory for crash buffer
 *
 * The PCI crash buffer may be allocated via vmalloc (for buffers
 * exceeding ~4 MB where the buddy allocator cannot provide contiguous
 * pages).  virt_to_phys() returns garbage for vmalloc addresses, so
 * we maintain this small kmalloc'd directory that maps the buffer's
 * virtual pages to their actual physical addresses.
 *
 * At panic time, crash_core.c exports the pagemap's physical address
 * via VMCOREINFO.  The parser reads the pagemap, then reads each
 * physical page from the vmcore to reconstruct the full buffer.
 *
 * The pagemap itself is always kmalloc'd (direct-mapped), so
 * virt_to_phys() works correctly on it.
 *
 * @magic:     0x5043504d ("PCPM") -- validates this is a pagemap
 * @num_pages: Number of entries in the addrs[] array
 * @buf_size:  Exact buffer size in bytes (last page may be partial)
 * @buf_offset: Offset of buffer start within the first page
 * @addrs:     Physical address of each PAGE_SIZE page backing the buffer
 */
struct pci_crash_pagemap {
	__le32 magic;
	__le32 num_pages;
	__le64 buf_size;
	__le32 buf_offset;
	__le64 addrs[];
} __packed;

#define PCI_CRASH_PAGEMAP_MAGIC  0x5043504d  /* "PCPM" in ASCII */

struct pci_bus;

#ifdef CONFIG_PCI_CRASH
void pci_crash_save(void);
extern void *pci_crash_buffer;
extern size_t pci_crash_buffer_size;
extern phys_addr_t pci_crash_pagemap_phys;

/*
 * Non-blocking config read used only by the crash capture path; defined in
 * drivers/pci/access.c (where pci_lock lives).  Not exported and not part of
 * the public PCI API -- it is specific to this feature.
 */
int pci_bus_read_config_dword_trylock(struct pci_bus *bus, unsigned int devfn,
				      int pos, u32 *value);
#else
static inline void pci_crash_save(void) {}
#define pci_crash_buffer        ((void *)NULL)
#define pci_crash_buffer_size   ((size_t)0)
#define pci_crash_pagemap_phys  ((phys_addr_t)0)
#endif

#endif /* _LINUX_PCI_CRASH_H */
