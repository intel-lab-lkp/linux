/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_TSM_H
#define __PCI_TSM_H
#include <linux/mutex.h>

struct pci_dev;

/**
 * struct pci_dsm - Device Security Manager context
 * @pdev: physical device back pointer
 */
struct pci_dsm {
	struct pci_dev *pdev;
};

enum pci_tsm_state {
	PCI_TSM_ERR = -1,
	PCI_TSM_INIT,
	PCI_TSM_CONNECT,
};

/**
 * struct pci_tsm - Platform TSM transport context
 * @state: reflect device initialized, connected, or bound
 * @lock: protect @state vs pci_tsm_ops invocation
 * @doe_mb: PCIe Data Object Exchange mailbox
 * @dsm: TSM driver device context established by pci_tsm_ops.probe
 */
struct pci_tsm {
	enum pci_tsm_state state;
	struct mutex lock;
	struct pci_doe_mb *doe_mb;
	struct pci_dsm *dsm;
};

/**
 * struct pci_tsm_ops - Low-level TSM-exported interface to the PCI core
 * @probe: probe/accept device for tsm operation, setup DSM context
 * @remove: destroy DSM context
 * @connect: establish / validate a secure connection (e.g. IDE) with the device
 * @disconnect: teardown the secure connection
 *
 * @probe and @remove run in pci_tsm_rwsem held for write context. All
 * other ops run under the @pdev->tsm->lock mutex and pci_tsm_rwsem held
 * for read.
 */
struct pci_tsm_ops {
	struct pci_dsm *(*probe)(struct pci_dev *pdev);
	void (*remove)(struct pci_dsm *dsm);
	int (*connect)(struct pci_dev *pdev);
	void (*disconnect)(struct pci_dev *pdev);
};

enum pci_doe_proto {
	PCI_DOE_PROTO_CMA = 1,
	PCI_DOE_PROTO_SSESSION = 2,
};

#ifdef CONFIG_PCI_TSM
int pci_tsm_register(const struct pci_tsm_ops *ops,
		     const struct attribute_group *grp);
void pci_tsm_unregister(const struct pci_tsm_ops *ops);
int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz);
#else
static inline int pci_tsm_register(const struct pci_tsm_ops *ops,
				   const struct attribute_group *grp)
{
	return 0;
}
static inline void pci_tsm_unregister(const struct pci_tsm_ops *ops)
{
}
static inline int pci_tsm_doe_transfer(struct pci_dev *pdev,
				       enum pci_doe_proto type, const void *req,
				       size_t req_sz, void *resp,
				       size_t resp_sz)
{
	return -ENOENT;
}
#endif
#endif /*__PCI_TSM_H */
