/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_TSM_H
#define __PCI_TSM_H
#include <linux/mutex.h>

enum pci_tsm_cmd {
	TSM_EXEC_CONNECT,
	TSM_EXEC_DISCONNECT,
};

struct pci_dev;
/**
 * struct pci_tsm_ops - Low-level TSM-exported interface to the PCI core
 * @add: accept device for tsm operation
 * @del: teardown tsm context for @pdev
 * @exec: synchronously execute @cmd
 *
 * Note that @add, and @del run in down_write(&pci_tsm_rswem) context to
 * synchronize with TSM driver bind/unbind events and
 * pci_device_add()/pci_destroy_dev(). @exec runs in
 * @pdev->tsm->exec_lock context to synchronize @exec results with
 * @pdev->tsm->state
 */
struct pci_tsm_ops {
	int (*add)(struct pci_dev *pdev);
	void (*del)(struct pci_dev *pdev);
	int (*exec)(struct pci_dev *pdev, enum pci_tsm_cmd cmd);
};

enum pci_tsm_state {
	PCI_TSM_IDLE,
	PCI_TSM_INIT,
	PCI_TSM_CONNECT,
};

/**
 * struct pci_tsm - per device TSM context
 * @state: reflect device initialized, connected, or bound
 * @ide_cap: PCIe IDE Extended Capability offset
 * @exec_lock: protect @state vs pci_tsm_ops.exec() results
 * @doe_mb: PCIe Data Object Exchange mailbox
 * @tsm_data: TSM driver private context
 */
struct pci_tsm {
	enum pci_tsm_state state;
	u16 ide_cap;
	struct mutex exec_lock;
	struct pci_doe_mb *doe_mb;
	void *tsm_data;
};

enum pci_doe_proto {
	PCI_DOE_PROTO_CMA = 1,
	PCI_DOE_PROTO_SSESSION = 2,
};

#ifdef CONFIG_PCI_TSM
int pci_tsm_register(const struct pci_tsm_ops *ops);
void pci_tsm_unregister(const struct pci_tsm_ops *ops);
int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz);
#else
static inline int pci_tsm_register(const struct pci_tsm_ops *ops)
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
