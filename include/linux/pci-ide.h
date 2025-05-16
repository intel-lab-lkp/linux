/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

/* PCIe 6.2 section 6.33 Integrity & Data Encryption (IDE) */

#ifndef __PCI_IDE_H__
#define __PCI_IDE_H__

#include <linux/range.h>

#define SEL_ADDR1_LOWER_MASK GENMASK(31, 20)
#define SEL_ADDR_UPPER_MASK GENMASK_ULL(63, 32)
#define PREP_PCI_IDE_SEL_ADDR1(base, limit)                    \
	(FIELD_PREP(PCI_IDE_SEL_ADDR_1_VALID, 1) |             \
	 FIELD_PREP(PCI_IDE_SEL_ADDR_1_BASE_LOW_MASK,          \
		    FIELD_GET(SEL_ADDR1_LOWER_MASK, (base))) | \
	 FIELD_PREP(PCI_IDE_SEL_ADDR_1_LIMIT_LOW_MASK,         \
		    FIELD_GET(SEL_ADDR1_LOWER_MASK, (limit))))

#define PREP_PCI_IDE_SEL_RID_2(base, domain)               \
	(FIELD_PREP(PCI_IDE_SEL_RID_2_VALID, 1) |          \
	 FIELD_PREP(PCI_IDE_SEL_RID_2_BASE_MASK, (base)) | \
	 FIELD_PREP(PCI_IDE_SEL_RID_2_SEG_MASK, (domain)))

enum pci_ide_partner_select {
	PCI_IDE_EP,
	PCI_IDE_RP,
	PCI_IDE_PARTNER_MAX,
	/* pci_ide_stream_alloc() uses this for stream index allocation */
	PCI_IDE_HB = PCI_IDE_PARTNER_MAX,
};

/**
 * struct pci_ide_partner - Per port IDE Stream settings
 * @rid_start: Partner Port Requester ID range start
 * @rid_start: Partner Port Requester ID range end
 * @stream_index: Selective IDE Stream Register Block selection
 */
struct pci_ide_partner {
	u16 rid_start;
	u16 rid_end;
	u8 stream_index;
};

/**
 * struct pci_ide - PCIe Selective IDE Stream descriptor
 * @pdev: PCIe Endpoint for the stream
 * @partner: settings for both partner ports in a stream
 * @host_bridge_stream: track platform Stream index
 * @stream_id: unique id (within Partner Port pairing) for the stream
 * @name: name of the stream in sysfs
 *
 * Negative @stream_id values indicate "uninitialized" on the
 * expectation that with TSM established IDE the TSM owns the stream_id
 * allocation.
 */
struct pci_ide {
	struct pci_dev *pdev;
	struct pci_ide_partner partner[PCI_IDE_PARTNER_MAX];
	u8 host_bridge_stream;
	int stream_id;
	const char *name;
};

int pci_ide_domain(struct pci_dev *pdev);
struct pci_ide_partner *pci_ide_to_settings(struct pci_dev *pdev, struct pci_ide *ide);
struct pci_ide *pci_ide_stream_alloc(struct pci_dev *pdev);
void pci_ide_stream_free(struct pci_ide *ide);
DEFINE_FREE(pci_ide_stream_free, struct pci_ide *, if (_T) pci_ide_stream_free(_T))
int  pci_ide_stream_register(struct pci_ide *ide);
void pci_ide_stream_unregister(struct pci_ide *ide);
void pci_ide_stream_setup(struct pci_dev *pdev, struct pci_ide *ide);
void pci_ide_stream_teardown(struct pci_dev *pdev, struct pci_ide *ide);
int pci_ide_stream_enable(struct pci_dev *pdev, struct pci_ide *ide);
void pci_ide_stream_disable(struct pci_dev *pdev, struct pci_ide *ide);
#endif /* __PCI_IDE_H__ */
