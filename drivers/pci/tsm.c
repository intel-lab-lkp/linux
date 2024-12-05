// SPDX-License-Identifier: GPL-2.0
/*
 * TEE Security Manager for the TEE Device Interface Security Protocol
 * (TDISP, PCIe r6.1 sec 11)
 *
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#define dev_fmt(fmt) "TSM: " fmt

#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/sysfs.h>
#include <linux/xarray.h>
#include <linux/pci-tsm.h>
#include <linux/bitfield.h>
#include "pci.h"

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a tsm instance
 */
static DECLARE_RWSEM(pci_tsm_rwsem);
static const struct pci_tsm_ops *tsm_ops;

/* supplemental attributes to surface when pci_tsm_attr_group is active */
static const struct attribute_group *pci_tsm_owner_attr_group;

static int pci_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held(&pci_tsm_rwsem);
	if_not_guard(mutex_intr, &pci_tsm->lock)
		return -EINTR;

	if (pci_tsm->state < PCI_TSM_CONNECT)
		return 0;
	if (pci_tsm->state < PCI_TSM_INIT)
		return -ENXIO;

	tsm_ops->disconnect(pdev);
	pci_tsm->state = PCI_TSM_INIT;

	return 0;
}

static int pci_tsm_connect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;
	int rc;

	lockdep_assert_held(&pci_tsm_rwsem);
	if_not_guard(mutex_intr, &pci_tsm->lock)
		return -EINTR;

	if (pci_tsm->state >= PCI_TSM_CONNECT)
		return 0;
	if (pci_tsm->state < PCI_TSM_INIT)
		return -ENXIO;

	rc = tsm_ops->connect(pdev);
	if (rc)
		return rc;
	pci_tsm->state = PCI_TSM_CONNECT;
	return 0;
}

static ssize_t connect_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	int rc;
	bool connect;
	struct pci_dev *pdev = to_pci_dev(dev);

	rc = kstrtobool(buf, &connect);
	if (rc)
		return rc;

	if_not_guard(rwsem_read_intr, &pci_tsm_rwsem)
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

	if_not_guard(rwsem_read_intr, &pci_tsm_rwsem)
		return -EINTR;
	if (!pdev->tsm)
		return -ENXIO;
	return sysfs_emit(buf, "%d\n", pdev->tsm->state >= PCI_TSM_CONNECT);
}
static DEVICE_ATTR_RW(connect);

static bool pci_tsm_group_visible(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->tsm)
		return true;
	return false;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_tsm);

static struct attribute *pci_tsm_attrs[] = {
	&dev_attr_connect.attr,
	NULL,
};

const struct attribute_group pci_tsm_attr_group = {
	.name = "tsm",
	.attrs = pci_tsm_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),
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
	NULL,
};

const struct attribute_group pci_tsm_auth_attr_group = {
	.attrs = pci_tsm_auth_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_tsm),
};

static void dsm_remove(struct pci_dsm *dsm)
{
	if (!dsm)
		return;
	tsm_ops->remove(dsm);
}
DEFINE_FREE(dsm_remove, struct pci_dsm *, if (_T) dsm_remove(_T))

static bool is_physical_endpoint(struct pci_dev *pdev)
{
	if (!pci_is_pcie(pdev))
		return false;

	if (pdev->is_virtfn)
		return false;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return false;

	return true;
}

static void __pci_tsm_init(struct pci_dev *pdev)
{
	bool tee_cap;

	if (!is_physical_endpoint(pdev))
		return;

	tee_cap = pdev->devcap & PCI_EXP_DEVCAP_TEE;

	if (!(pdev->ide_cap || tee_cap))
		return;

	lockdep_assert_held_write(&pci_tsm_rwsem);
	if (!tsm_ops)
		return;

	struct pci_tsm *pci_tsm __free(kfree) = kzalloc(sizeof(*pci_tsm), GFP_KERNEL);
	if (!pci_tsm)
		return;

	/*
	 * If a physical device has any security capabilities it may be
	 * a candidate to connect with the platform TSM
	 */
	struct pci_dsm *dsm __free(dsm_remove) = tsm_ops->probe(pdev);

	pci_dbg(pdev, "Device security capabilities detected (%s%s ), TSM %s\n",
		pdev->ide_cap ? " ide" : "", tee_cap ? " tee" : "",
		dsm ? "attach" : "skip");

	if (!dsm)
		return;

	mutex_init(&pci_tsm->lock);
	pci_tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					       PCI_DOE_PROTO_CMA);
	if (!pci_tsm->doe_mb) {
		pci_warn(pdev, "TSM init failure, no CMA mailbox\n");
		return;
	}

	pci_tsm->state = PCI_TSM_INIT;
	pci_tsm->dsm = no_free_ptr(dsm);
	pdev->tsm = no_free_ptr(pci_tsm);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	if (pci_tsm_owner_attr_group)
		sysfs_merge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
}

void pci_tsm_init(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_init(pdev);
}

int pci_tsm_register(const struct pci_tsm_ops *ops, const struct attribute_group *grp)
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
EXPORT_SYMBOL_GPL(pci_tsm_register);

static void __pci_tsm_destroy(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	if (!pci_tsm)
		return;

	lockdep_assert_held_write(&pci_tsm_rwsem);
	if (pci_tsm->state > PCI_TSM_INIT)
		pci_tsm_disconnect(pdev);
	tsm_ops->remove(pci_tsm->dsm);
	pdev->tsm = NULL;
	if (pci_tsm_owner_attr_group)
		sysfs_unmerge_group(&pdev->dev.kobj, pci_tsm_owner_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_auth_attr_group);
	kfree(pci_tsm);
}

void pci_tsm_destroy(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	__pci_tsm_destroy(pdev);
}

void pci_tsm_unregister(const struct pci_tsm_ops *ops)
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
EXPORT_SYMBOL_GPL(pci_tsm_unregister);

int pci_tsm_doe_transfer(struct pci_dev *pdev, enum pci_doe_proto type,
			 const void *req, size_t req_sz, void *resp,
			 size_t resp_sz)
{
	if (!pdev->tsm || !pdev->tsm->doe_mb)
		return -ENXIO;

	return pci_doe(pdev->tsm->doe_mb, PCI_VENDOR_ID_PCI_SIG, type, req,
		       req_sz, resp, resp_sz);
}
EXPORT_SYMBOL_GPL(pci_tsm_doe_transfer);
