/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data Object Exchange
 *	PCIe r6.0, sec 6.30 DOE
 *
 * Copyright (C) 2021 Huawei
 *     Jonathan Cameron <Jonathan.Cameron@huawei.com>
 *
 * Copyright (C) 2022 Intel Corporation
 *	Ira Weiny <ira.weiny@intel.com>
 */

#ifndef LINUX_PCI_DOE_H
#define LINUX_PCI_DOE_H

#include <linux/types.h>

/* Mailbox state flags */
#define PCI_DOE_FLAG_CANCEL		0
#define PCI_DOE_FLAG_DEAD		1

/* Max data object length is 2^18 dwords */
#define PCI_DOE_MAX_LENGTH		(1 << 18)

#define PCI_DOE_FEATURE_DISCOVERY	0
#define PCI_DOE_FEATURE_CMA		1
#define PCI_DOE_FEATURE_SSESSION	2

typedef int (*pci_doe_protocol_handler_t)(const void *request,
					  size_t request_sz,
					  void **response,
					  size_t *response_sz);

struct pci_doe_feature {
	u16 vid;
	u8 type;
};

struct pci_doe_mb;
struct pci_dev;
struct pci_epc;

#ifdef CONFIG_PCI_DOE
struct pci_doe_mb *pci_find_doe_mailbox(struct pci_dev *pdev, u16 vendor,
					u8 type);

int pci_doe(struct pci_doe_mb *doe_mb, u16 vendor, u8 type,
	    const void *request, size_t request_sz,
	    void *response, size_t response_sz);

#else
static inline struct pci_doe_mb *pci_find_doe_mailbox(struct pci_dev *pdev,
						      u16 vendor, u8 type)
{
	return NULL;
}

static inline int pci_doe(struct pci_doe_mb *doe_mb, u16 vendor, u8 type,
			  const void *request, size_t request_sz,
			  void *response, size_t response_sz)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_PCI_DOE */

#ifdef CONFIG_PCI_DOE_EP
int pci_ep_doe_init(struct pci_epc *epc);

int pci_ep_doe_add_mailbox(struct pci_epc *epc, u8 func_no, u16 cap_offset);

int pci_ep_doe_register_protocol(u16 vendor, u8 type,
				 pci_doe_protocol_handler_t handler);

void pci_ep_doe_unregister_protocol(u16 vendor, u8 type);

int pci_ep_doe_process_request(struct pci_epc *epc, u8 func_no, u16 cap_offset,
			       u16 vendor, u8 type, const void *request,
			       size_t request_sz, void **response, size_t *response_sz);

int pci_ep_doe_abort(struct pci_epc *epc, u8 func_no, u16 cap_offset);

void pci_ep_doe_destroy(struct pci_epc *epc);

#else
static inline int pci_ep_doe_init(struct pci_epc *epc)
{
	return -EOPNOTSUPP;
}

static inline int pci_ep_doe_add_mailbox(struct pci_epc *epc, u8 func_no,
					 u16 cap_offset)
{
	return -EOPNOTSUPP;
}

static inline int pci_ep_doe_register_protocol(u16 vendor, u8 type,
					       pci_doe_protocol_handler_t handler)
{
	return -EOPNOTSUPP;
}

static inline void pci_ep_doe_unregister_protocol(u16 vendor, u8 type)
{
}

static inline int pci_ep_doe_process_request(struct pci_epc *epc, u8 func_no, u16 cap_offset,
					     u16 vendor, u8 type,
					     const void *request, size_t request_sz,
					     void **response, size_t *response_sz)
{
	return -EOPNOTSUPP;
}

static inline int pci_ep_doe_abort(struct pci_epc *epc, u8 func_no,
				   u16 cap_offset)
{
	return -EOPNOTSUPP;
}

static inline void pci_ep_doe_destroy(struct pci_epc *epc)
{
}

#endif /* CONFIG_PCI_DOE_EP */

#endif /* LINUX_PCI_DOE_H */
