/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_TSM_H
#define __PCI_TSM_H
#include <linux/mutex.h>
#include <linux/pci.h>

struct pci_dev;
struct kvm;

enum pci_tsm_state {
	PCI_TSM_ERR = -1,
	PCI_TSM_INIT,
	PCI_TSM_CONNECT,
};

/**
 * enum pci_tsm_type - 'struct pci_tsm' object types
 * @PCI_TSM_PF0: function0 that hosts a DOE mailbox that comprehends an
 *		 Interface ID per potential TDI
 * @PCI_TSM_VIRTFN: physfn-0 of this device is "tsm_pf0"
 * @PCI_TSM_MFD: function0 of this device is  "tsm_pf0"
 * @PCI_TSM_DOWNSTREAM: immediate Upstream Port of this device is "tsm_pf0"
 */
enum pci_tsm_type {
	PCI_TSM_INVALID,
	PCI_TSM_PF0,
	PCI_TSM_VIRTFN,
	PCI_TSM_MFD,
	PCI_TSM_DOWNSTREAM,
};

/**
 * struct pci_tdi - TDI context
 * @pdev: host side representation of guest-side TDI
 * @dsm: PF0 PCI device that can modify TDISP state for the TDI
 * @kvm: TEE VM context of bound TDI
 */
struct pci_tdi {
	struct pci_dev *pdev;
	struct pci_dev *dsm;
	struct kvm *kvm;
};

/**
 * struct pci_tsm - Core TSM context for a given PCIe endpoint
 * @pdev: indicates the type of pci_tsm object
 * @type: pci_tsm object type to disambiguate PCI_TSM_DOWNSTREAM and PCI_TSM_PF0
 * @tdi: TDI context
 *
 * This structure is wrapped by a low level TSM driver and returned by
 * tsm_ops.probe(), it is freed by tsm_ops.remove(). Depending on
 * whether @pdev is physical function 0, another physical function, or a
 * virtual function determines the pci_tsm object type. E.g. see 'struct
 * pci_tsm_pf0'.
 */
struct pci_tsm {
	struct pci_dev *pdev;
	enum pci_tsm_type type;
	struct pci_tdi *tdi;
};

/**
 * struct pci_tsm_pf0 - Physical Function 0 TDISP context
 * @state: reflect device initialized, connected, or bound
 * @lock: protect @state vs pci_tsm_ops invocation
 * @doe_mb: PCIe Data Object Exchange mailbox
 */
struct pci_tsm_pf0 {
	struct pci_tsm tsm;
	enum pci_tsm_state state;
	struct mutex lock;
	struct pci_doe_mb *doe_mb;
};

/* physical function0 and capable of 'connect' */
static inline bool is_pci_tsm_pf0(struct pci_dev *pdev)
{
	if (!pci_is_pcie(pdev))
		return false;

	if (pdev->is_virtfn)
		return false;

	/*
	 * Allow for a Device Security Manager (DSM) associated with function0
	 * of an Endpoint to coordinate TDISP requests for other functions
	 * (physical or virtual) of the device, or allow for an Upstream Port
	 * DSM to accept TDISP requests for switch Downstream Endpoints.
	 */
	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_RC_END:
		if (pdev->ide_cap || (pdev->devcap & PCI_EXP_DEVCAP_TEE))
			break;
		fallthrough;
	default:
		return false;
	}

	return PCI_FUNC(pdev->devfn) == 0;
}

enum pci_tsm_guest_req_type {
	PCI_TSM_GUEST_REQ_TDXC,
};

/**
 * struct pci_tsm_guest_req_info - parameter for pci_tsm_ops.guest_req()
 * @type: identify the format of the following blobs
 * @type_info: extra input/output info, e.g. firmware error code
 * @type_info_len: the size of @type_info
 * @req: request data buffer filled by guest
 * @req_len: the size of @req filled by guest
 * @resp: response data buffer filled by host
 * @resp_len: for input, the size of @resp buffer filled by guest
 *	      for output, the size of actual response data filled by host
 */
struct pci_tsm_guest_req_info {
	enum pci_tsm_guest_req_type type;
	void *type_info;
	size_t type_info_len;
	void *req;
	size_t req_len;
	void *resp;
	size_t resp_len;
};

/**
 * struct pci_tsm_ops - Low-level TSM-exported interface to the PCI core
 * @probe: probe/accept device for tsm operation, setup DSM context
 * @remove: destroy DSM context
 * @connect: establish / validate a secure connection (e.g. IDE) with the device
 * @disconnect: teardown the secure connection
 * @bind: establish a secure binding with the TVM
 * @unbind: teardown the secure binding
 * @guest_req: handle the vendor specific requests from TVM when bound
 *
 * @probe and @remove run in pci_tsm_rwsem held for write context. All
 * other ops run under the @pdev->tsm->lock mutex and pci_tsm_rwsem held
 * for read.
 */
struct pci_tsm_ops {
	struct pci_tsm *(*probe)(struct pci_dev *pdev);
	void (*remove)(struct pci_tsm *tsm);
	int (*connect)(struct pci_dev *pdev);
	void (*disconnect)(struct pci_dev *pdev);
	struct pci_tdi *(*bind)(struct pci_dev *pdev, struct pci_dev *pf0_dev,
				struct kvm *kvm, u64 tdi_id);
	void (*unbind)(struct pci_tdi *tdi);
	int (*guest_req)(struct pci_dev *pdev,
			 struct pci_tsm_guest_req_info *info);
};

enum pci_doe_proto {
	PCI_DOE_PROTO_CMA = 1,
	PCI_DOE_PROTO_SSESSION = 2,
};

#ifdef CONFIG_PCI_TSM
int pci_tsm_core_register(const struct pci_tsm_ops *ops,
			  const struct attribute_group *grp);
void pci_tsm_core_unregister(const struct pci_tsm_ops *ops);
int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz);
void pci_tsm_initialize(struct pci_dev *pdev, struct pci_tsm *tsm);
int pci_tsm_pf0_initialize(struct pci_dev *pdev, struct pci_tsm_pf0 *tsm);
int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u64 tdi_id);
int pci_tsm_unbind(struct pci_dev *pdev);
int pci_tsm_guest_req(struct pci_dev *pdev, struct pci_tsm_guest_req_info *info);
#else
static inline int pci_tsm_core_register(const struct pci_tsm_ops *ops,
					const struct attribute_group *grp)
{
	return 0;
}
static inline void pci_tsm_core_unregister(const struct pci_tsm_ops *ops)
{
}
static inline int pci_tsm_doe_transfer(struct pci_dev *pdev,
				       enum pci_doe_proto type, const void *req,
				       size_t req_sz, void *resp,
				       size_t resp_sz)
{
	return -ENOENT;
}
static inline int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm,
			       u64 tdi_id)
{
	return -ENOENT;
}
static inline int pci_tsm_unbind(struct pci_dev *pdev)
{
	return -ENOENT;
}
int pci_tsm_guest_req(struct pci_dev *pdev, struct pci_tsm_guest_req_info *info)
{
	return -ENOENT;
}
#endif
#endif /*__PCI_TSM_H */
