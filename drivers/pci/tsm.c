// SPDX-License-Identifier: GPL-2.0
/*
 * TEE Security Manager for the TEE Device Interface Security Protocol
 * (TDISP, PCIe r6.1 sec 11)
 *
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#define dev_fmt(fmt) "TSM: " fmt

#include <linux/bitfield.h>
#include <linux/xarray.h>
#include <linux/sysfs.h>

#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/pci-tsm.h>
#include "pci.h"

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a tsm instance
 */
static DECLARE_RWSEM(pci_tsm_rwsem);
static const struct pci_tsm_ops *tsm_ops;

/* supplemental attributes to surface when pci_tsm_attr_group is active */
static const struct attribute_group *pci_tsm_owner_attr_group;

static struct pci_tsm_pf0 *to_pci_tsm_pf0(struct pci_tsm *pci_tsm)
{
	struct pci_dev *pdev = pci_tsm->pdev;

	if (!is_pci_tsm_pf0(pdev) || pci_tsm->type != PCI_TSM_PF0) {
		dev_WARN_ONCE(&pdev->dev, 1, "invalid context object\n");
		return NULL;
	}

	return container_of(pci_tsm, struct pci_tsm_pf0, tsm);
}

/* TODO: switch to ACQUIRE() and ACQUIRE_ERR() */
static struct mutex *tsm_ops_lock(struct pci_tsm_pf0 *tsm)
{
	lockdep_assert_held(&pci_tsm_rwsem);

	if (mutex_lock_interruptible(&tsm->lock) != 0)
		return NULL;
	return &tsm->lock;
}
DEFINE_FREE(tsm_ops_unlock, struct mutex *, if (_T) mutex_unlock(_T))

static int __pci_tsm_unbind(struct pci_dev *pdev);
static void pci_tsm_unbind_all_vfs(struct pci_dev *pdev)
{
	struct pci_dev *virtfn;

	for (int i = 0; i < pci_num_vf(pdev); i++) {
		virtfn = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),
						     pci_iov_virtfn_bus(pdev, i),
						     pci_iov_virtfn_devfn(pdev, i));
		if (virtfn) {
			__pci_tsm_unbind(virtfn);
			pci_dev_put(virtfn);
		}
	}
}

static void pci_tsm_unbind_all_mfds(struct pci_dev *pdev)
{
	struct pci_dev *phyfn;

	for (int i = 0; i < 8; i++) {
		phyfn = pci_get_slot(pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), i));
		if (phyfn) {
			__pci_tsm_unbind(phyfn);
			pci_dev_put(phyfn);
		}
	}
}

static int unbind_downstream(struct pci_dev *pdev, void *uport_subordinate)
{
	if (pdev->bus->parent != uport_subordinate)
		return 0;

	if (pdev->tsm && pdev->tsm->type == PCI_TSM_DOWNSTREAM)
		__pci_tsm_unbind(pdev);

	return 0;
}

static void pci_tsm_unbind_all_downstream(struct pci_dev *pdev)
{
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_UPSTREAM)
		return;

	if (!pdev->tsm)
		return;

	pci_walk_bus(pdev->subordinate, unbind_downstream, pdev->subordinate);
}

static int pci_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_tsm_pf0 *tsm = to_pci_tsm_pf0(pdev->tsm);

	pci_tsm_unbind_all_downstream(pdev);
	pci_tsm_unbind_all_vfs(pdev);
	pci_tsm_unbind_all_mfds(pdev);

	struct mutex *lock __free(tsm_ops_unlock) = tsm_ops_lock(tsm);
	if (!lock)
		return -EINTR;

	if (tsm->state < PCI_TSM_INIT)
		return -ENXIO;
	if (tsm->state < PCI_TSM_CONNECT)
		return 0;

	tsm_ops->disconnect(pdev);
	tsm->state = PCI_TSM_INIT;

	return 0;
}

/*
 * TDISP locked state temporarily makes the device inaccessible, do not
 * surprise live attached drivers
 */
static int __driver_idle_connect(struct pci_dev *pdev)
{
	guard(device)(&pdev->dev);
	if (pdev->dev.driver)
		return -EBUSY;
	return tsm_ops->connect(pdev);
}

/*
 * When the registered ops support accept it indicates that this is a
 * TVM-side (guest) TSM operations structure. In this mode ->connect()
 * arranges for the TDI to enter TDISP LOCKED state, and ->accept()
 * transitions the device to RUN state.
 */
static bool tvm_mode(void)
{
	return !!tsm_ops->accept;
}

static int pci_tsm_connect(struct pci_dev *pdev)
{
	struct pci_tsm_pf0 *tsm = to_pci_tsm_pf0(pdev->tsm);
	int rc;

	struct mutex *lock __free(tsm_ops_unlock) = tsm_ops_lock(tsm);
	if (!lock)
		return -EINTR;

	if (tsm->state < PCI_TSM_INIT)
		return -ENXIO;
	if (tsm->state >= PCI_TSM_CONNECT)
		return 0;

	if (tvm_mode())
		rc = __driver_idle_connect(pdev);
	else
		rc = tsm_ops->connect(pdev);
	if (rc)
		return rc;
	tsm->state = PCI_TSM_CONNECT;
	return 0;
}

static int pci_tsm_accept(struct pci_dev *pdev)
{
	struct pci_tsm_pf0 *tsm = to_pci_tsm_pf0(pdev->tsm);
	int rc;

	struct mutex *lock __free(tsm_ops_unlock) = tsm_ops_lock(tsm);
	if (!lock)
		return -EINTR;

	if (tsm->state < PCI_TSM_CONNECT)
		return -ENXIO;
	if (tsm->state >= PCI_TSM_ACCEPT)
		return 0;

	/*
	 * "Accept" transitions a device to the run state, it is only suitable
	 * to make that transition from a known DMA-idle (no active mappings)
	 * state.  The "driver detached" state is a coarse way to assert that
	 * requirement.
	 */
	guard(device)(&pdev->dev);
	if (pdev->dev.driver)
		return -EBUSY;

	rc = tsm_ops->accept(pdev);
	if (rc)
		return rc;
	tsm->state = PCI_TSM_ACCEPT;
	return 0;
}

/* TODO: switch to ACQUIRE() and ACQUIRE_ERR() */
static struct rw_semaphore *tsm_read_lock(void)
{
	if (down_read_interruptible(&pci_tsm_rwsem))
		return NULL;
	return &pci_tsm_rwsem;
}
DEFINE_FREE(tsm_read_unlock, struct rw_semaphore *, if (_T) up_read(_T))

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	int rc;
	bool connect;
	struct pci_dev *pdev = to_pci_dev(dev);

	rc = kstrtobool(buf, &connect);
	if (rc)
		return rc;

	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	if (connect)
		rc = pci_tsm_connect(pdev);
	else
		rc = pci_tsm_disconnect(pdev);
	if (rc)
		return rc;
	return len;
}

static ssize_t connect_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_tsm_pf0 *tsm;

	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	if (!pdev->tsm)
		return -ENXIO;

	tsm = to_pci_tsm_pf0(pdev->tsm);
	return sysfs_emit(buf, "%d\n", tsm->state >= PCI_TSM_CONNECT);
}
static DEVICE_ATTR_RW(connect);

static ssize_t accept_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	bool accept;
	int rc;

	rc = kstrtobool(buf, &accept);
	if (rc)
		return rc;

	if (!accept)
		return -EINVAL;

	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	rc = pci_tsm_accept(pdev);
	if (rc)
		return rc;

	return len;
}

static ssize_t accept_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_tsm_pf0 *tsm;

	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	if (!pdev->tsm)
		return -ENXIO;

	tsm = to_pci_tsm_pf0(pdev->tsm);
	return sysfs_emit(buf, "%d\n", tsm->state >= PCI_TSM_ACCEPT);
}
static DEVICE_ATTR_RW(accept);

static umode_t pci_tsm_pf0_attr_visible(struct kobject *kobj,
					struct attribute *a, int n)
{
	if (!tvm_mode()) {
		/* Host context, filter out guest only attributes */
		if (a == &dev_attr_accept.attr)
			return 0;
	}

	return a->mode;
}

static bool pci_tsm_pf0_group_visible(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->tsm && is_pci_tsm_pf0(pdev))
		return true;
	return false;
}
DEFINE_SYSFS_GROUP_VISIBLE(pci_tsm_pf0);

static struct attribute *pci_tsm_pf0_attrs[] = {
	&dev_attr_connect.attr,
	&dev_attr_accept.attr,
	NULL
};

const struct attribute_group pci_tsm_pf0_attr_group = {
	.name = "tsm",
	.attrs = pci_tsm_pf0_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm_pf0),
};

static ssize_t authenticated_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	/*
	 * When device authentication is TSM owned, 'authenticated' is
	 * identical to the connect state.
	 */
	return connect_show(dev, attr, buf);
}
static DEVICE_ATTR_RO(authenticated);

static struct attribute *pci_tsm_auth_attrs[] = {
	&dev_attr_authenticated.attr,
	NULL
};

const struct attribute_group pci_tsm_auth_attr_group = {
	.attrs = pci_tsm_auth_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm_pf0),
};

/*
 * Retrieve physical function0 device whether it has TEE capability or not
 */
static struct pci_dev *pf0_dev_get(struct pci_dev *pdev)
{
	struct pci_dev *pf_dev = pci_physfn(pdev);

	if (PCI_FUNC(pf_dev->devfn) == 0)
		return pci_dev_get(pf_dev);

	return pci_get_slot(pf_dev->bus,
			    pf_dev->devfn - PCI_FUNC(pf_dev->devfn));
}

static bool is_pci_tsm_downstream(struct pci_dev *pdev)
{
	struct pci_dev *uport;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return false;

	/* "grandparent" of an endpoint is an Upstream Port (or Root Port) */
	if (!pdev->dev.parent)
		return false;
	if (!pdev->dev.parent->parent)
		return false;

	uport = to_pci_dev(pdev->dev.parent->parent);
	if (pci_pcie_type(uport) != PCI_EXP_TYPE_UPSTREAM)
		return false;

	if (!uport->tsm)
		return false;

	/* Upstream Port has a 'tsm' context, probe downstream devices. */
	return true;
}

static enum pci_tsm_type pci_tsm_type(struct pci_dev *pdev)
{
	if (is_pci_tsm_pf0(pdev))
		return PCI_TSM_PF0;

	struct pci_dev *pf0 __free(pci_dev_put) = pf0_dev_get(pdev);
	if (!pf0)
		return PCI_TSM_INVALID;

	if (pf0->tsm && pf0->tsm->type == PCI_TSM_PF0) {
		if (pdev->is_virtfn)
			return PCI_TSM_VIRTFN;
		else
			return PCI_TSM_MFD;
	}

	/*
	 * Allow for Device Security Managers (DSMs) at a Switch level
	 * to host TDISP services for downstream devices
	 */
	if (is_pci_tsm_downstream(pdev))
		return PCI_TSM_DOWNSTREAM;
	return PCI_TSM_INVALID;
}

/**
 * pci_tsm_initialize() - base 'struct pci_tsm' initialization
 * @pdev: The PCI device
 * @tsm: context to initialize
 */
void pci_tsm_initialize(struct pci_dev *pdev, struct pci_tsm *tsm)
{
	tsm->type = pci_tsm_type(pdev);
	tsm->pdev = pdev;
}
EXPORT_SYMBOL_GPL(pci_tsm_initialize);

/**
 * pci_tsm_pf0_initialize() - common 'struct pci_tsm_pf0' initialization
 * @pdev: Physical Function 0 PCI device
 * @tsm: context to initialize
 */
int pci_tsm_pf0_initialize(struct pci_dev *pdev, struct pci_tsm_pf0 *tsm)
{
	mutex_init(&tsm->lock);

	/* Assigned pci device in guest won't have IDE and DOE exposed. */
	if (!tvm_mode()) {
		tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
						   PCI_DOE_PROTO_CMA);
		if (!tsm->doe_mb) {
			pci_warn(pdev, "TSM init failure, no CMA mailbox\n");
			return -ENODEV;
		}
	}

	tsm->state = PCI_TSM_INIT;
	pci_tsm_initialize(pdev, &tsm->tsm);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_pf0_initialize);

static void __pci_tsm_pf0_destroy(struct pci_tsm_pf0 *tsm)
{
	mutex_destroy(&tsm->lock);
}

static void tsm_remove(struct pci_tsm *tsm)
{
	if (!tsm)
		return;
	tsm_ops->remove(tsm);
}
DEFINE_FREE(tsm_remove, struct pci_tsm *, if (_T) tsm_remove(_T))

static void pci_tsm_pf0_init(struct pci_dev *pdev)
{
	bool tee_cap;

	tee_cap = pdev->devcap & PCI_EXP_DEVCAP_TEE;

	if (!(pdev->ide_cap || tee_cap))
		return;

	lockdep_assert_held_write(&pci_tsm_rwsem);
	if (!tsm_ops)
		return;

	/*
	 * If a physical device has any security capabilities it may be
	 * a candidate to connect with the platform TSM
	 */
	struct pci_tsm *pci_tsm __free(tsm_remove) = tsm_ops->probe(pdev);

	pci_dbg(pdev, "Device security capabilities detected (%s%s ), TSM %s\n",
		pdev->ide_cap ? " ide" : "", tee_cap ? " tee" : "",
		pci_tsm ? "attach" : "skip");

	if (!pci_tsm)
		return;

	pdev->tsm = no_free_ptr(pci_tsm);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_pf0_attr_group);
	if (pci_tsm_owner_attr_group)
		sysfs_merge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
}

static void __pci_tsm_init(struct pci_dev *pdev)
{
	enum pci_tsm_type type = pci_tsm_type(pdev);

	switch (type) {
	case PCI_TSM_PF0:
		pci_tsm_pf0_init(pdev);
		break;
	case PCI_TSM_VIRTFN:
	case PCI_TSM_MFD:
	case PCI_TSM_DOWNSTREAM:
		pdev->tsm = tsm_ops->probe(pdev);
		break;
	case PCI_TSM_INVALID:
	default:
		break;
	}
}

void pci_tsm_init(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_init(pdev);
}

int pci_tsm_core_register(const struct pci_tsm_ops *ops, const struct attribute_group *grp)
{
	struct pci_dev *pdev = NULL;

	if (!ops)
		return 0;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (tsm_ops)
		return -EBUSY;
	tsm_ops = ops;
	pci_tsm_owner_attr_group = grp;
	for_each_pci_dev(pdev)
		__pci_tsm_init(pdev);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_core_register);

static void pci_tsm_pf0_destroy(struct pci_dev *pdev)
{
	struct pci_tsm_pf0 *tsm = to_pci_tsm_pf0(pdev->tsm);

	if (tsm->state > PCI_TSM_INIT)
		pci_tsm_disconnect(pdev);
	pdev->tsm = NULL;
	if (pci_tsm_owner_attr_group)
		sysfs_unmerge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_pf0_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	__pci_tsm_pf0_destroy(tsm);
}

static void __pci_tsm_destroy(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	if (!pci_tsm)
		return;

	lockdep_assert_held_write(&pci_tsm_rwsem);

	if (is_pci_tsm_pf0(pdev)) {
		pci_tsm_pf0_destroy(pdev);
	} else {
		__pci_tsm_unbind(pdev);
		pdev->tsm = NULL;
	}
	tsm_ops->remove(pci_tsm);
}

void pci_tsm_destroy(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_destroy(pdev);
}

void pci_tsm_core_unregister(const struct pci_tsm_ops *ops)
{
	struct pci_dev *pdev = NULL;

	if (!ops)
		return;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (ops != tsm_ops)
		return;
	for_each_pci_dev(pdev)
		__pci_tsm_destroy(pdev);
	tsm_ops = NULL;
}
EXPORT_SYMBOL_GPL(pci_tsm_core_unregister);

int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz)
{
	struct pci_tsm_pf0 *tsm;

	if (!pdev->tsm || !is_pci_tsm_pf0(pdev) || tvm_mode())
		return -ENXIO;

	tsm = to_pci_tsm_pf0(pdev->tsm);
	if (!tsm->doe_mb)
		return -ENXIO;

	return pci_doe(tsm->doe_mb, PCI_VENDOR_ID_PCI_SIG, type, req, req_sz,
		       resp, resp_sz);
}
EXPORT_SYMBOL_GPL(pci_tsm_doe_transfer);

/* lookup the Device Security Manager (DSM) pf0 for @pdev */
static struct pci_dev *dsm_dev_get(struct pci_dev *pdev)
{
	struct pci_dev *uport_pf0;

	struct pci_dev *pf0 __free(pci_dev_put) = pf0_dev_get(pdev);
	if (!pf0)
		return NULL;

	/* Check that @pf0 was not initialized as PCI_TSM_DOWNSTREAM */
	if (pf0->tsm && pf0->tsm->type == PCI_TSM_PF0)
		return no_free_ptr(pf0);

	/*
	 * For cases where a switch may be hosting TDISP services on
	 * behalf of downstream devices, check the first usptream port
	 * relative to this endpoint.
	 */
	if (!pdev->dev.parent || !pdev->dev.parent->parent)
		return NULL;

	uport_pf0 = to_pci_dev(pdev->dev.parent->parent);
	if (!uport_pf0->tsm)
		return NULL;
	return pci_dev_get(uport_pf0);
}

/* Only implement non-interruptible lock for now */
static struct mutex *tdi_ops_lock(struct pci_dev *pf0_dev)
{
	struct pci_tsm_pf0 *pf0_tsm;

	lockdep_assert_held(&pci_tsm_rwsem);

	if (!pf0_dev->tsm)
		return ERR_PTR(-EINVAL);

	pf0_tsm = to_pci_tsm_pf0(pf0_dev->tsm);
	mutex_lock(&pf0_tsm->lock);

	if (pf0_tsm->state < PCI_TSM_CONNECT) {
		mutex_unlock(&pf0_tsm->lock);
		return ERR_PTR(-EINVAL);
	}

	return &pf0_tsm->lock;
}
DEFINE_FREE(tdi_ops_unlock, struct mutex *, if (!IS_ERR(_T)) mutex_unlock(_T))

int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u64 tdi_id)
{
	struct pci_tdi *tdi;

	if (!kvm)
		return -EINVAL;

	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	if (!pdev->tsm)
		return -EINVAL;

	struct pci_dev *dsm_dev __free(pci_dev_put) = dsm_dev_get(pdev);
	if (!dsm_dev)
		return -EINVAL;

	struct mutex *ops_lock __free(tdi_ops_unlock) = tdi_ops_lock(dsm_dev);
	if (IS_ERR(ops_lock))
		return PTR_ERR(ops_lock);

	if (pdev->tsm->tdi) {
		if (pdev->tsm->tdi->kvm == kvm)
			return 0;
		else
			return -EBUSY;
	}

	tdi = tsm_ops->bind(pdev, dsm_dev, kvm, tdi_id);
	if (!tdi)
		return -ENXIO;

	tdi->pdev = pdev;
	tdi->dsm_dev = dsm_dev;
	tdi->kvm = kvm;
	pdev->tsm->tdi = tdi;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_bind);

static int __pci_tsm_unbind(struct pci_dev *pdev)
{
	struct pci_tdi *tdi;

	lockdep_assert_held(&pci_tsm_rwsem);

	if (!pdev->tsm)
		return -EINVAL;

	struct pci_dev *dsm_dev __free(pci_dev_put) = dsm_dev_get(pdev);
	if (!dsm_dev)
		return -EINVAL;

	struct mutex *lock __free(tdi_ops_unlock) = tdi_ops_lock(dsm_dev);
	if (IS_ERR(lock))
		return PTR_ERR(lock);

	tdi = pdev->tsm->tdi;
	if (!tdi)
		return 0;

	tsm_ops->unbind(tdi);
	pdev->tsm->tdi = NULL;

	return 0;
}

int pci_tsm_unbind(struct pci_dev *pdev)
{
	struct rw_semaphore *lock __free(tsm_read_unlock) = tsm_read_lock();
	if (!lock)
		return -EINTR;

	return __pci_tsm_unbind(pdev);
}
EXPORT_SYMBOL_GPL(pci_tsm_unbind);

/**
 * pci_tsm_guest_req - VFIO/IOMMUFD helper to handle guest requests
 * @pdev: @pdev representing a bound tdi
 * @info: envelope for the request
 *
 * Expected flow is guest low-level TSM driver initiates a guest request
 * like "transition TDISP state to RUN", "fetch report" via a
 * technology specific guest-host-interface and KVM exit reason. KVM
 * posts to userspace (e.g. QEMU) that holds the host-to-guest RID
 * mapping.
 */
int pci_tsm_guest_req(struct pci_dev *pdev, struct pci_tsm_guest_req_info *info)
{
	struct pci_tdi *tdi;
	int rc;

	lockdep_assert_held_read(&pci_tsm_rwsem);

	if (!pdev->tsm)
		return -ENODEV;

	struct pci_dev *dsm_dev __free(pci_dev_put) = dsm_dev_get(pdev);
	if (!dsm_dev)
		return -EINVAL;

	struct mutex *lock __free(tdi_ops_unlock) = tdi_ops_lock(dsm_dev);
	if (IS_ERR(lock))
		return -ENODEV;

	tdi = pdev->tsm->tdi;
	if (!tdi)
		return -ENODEV;

	rc = tsm_ops->guest_req(pdev, info);
	if (rc)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_guest_req);
