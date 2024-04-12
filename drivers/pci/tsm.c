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

/* collect TSM capable devices to rendezvous with the tsm driver */
static DEFINE_XARRAY(pci_tsm_devs);

/*
 * Provide a read/write lock against the init / exit of pdev tsm
 * capabilities and arrival/departure of a tsm instance
 */
static DECLARE_RWSEM(pci_tsm_rwsem);
static const struct pci_tsm_ops *tsm_ops;

static int pci_tsm_disconnect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held_read(&pci_tsm_rwsem);
	scoped_cond_guard(mutex_intr, return -EINTR, &pci_tsm->exec_lock) {
		int rc;

		if (pci_tsm->state < PCI_TSM_CONNECT)
			return 0;
		if (pci_tsm->state < PCI_TSM_INIT)
			return -ENXIO;

		rc = tsm_ops->exec(pdev, TSM_EXEC_DISCONNECT);
		if (rc)
			return rc;
		pci_tsm->state = PCI_TSM_INIT;
	}
	return 0;
}

static int pci_tsm_connect(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held_read(&pci_tsm_rwsem);
	scoped_cond_guard(mutex_intr, return -EINTR, &pci_tsm->exec_lock) {
		int rc;

		if (pci_tsm->state >= PCI_TSM_CONNECT)
			return 0;
		if (pci_tsm->state < PCI_TSM_INIT)
			return -ENXIO;

		rc = tsm_ops->exec(pdev, TSM_EXEC_CONNECT);
		if (rc)
			return rc;
		pci_tsm->state = PCI_TSM_CONNECT;
	}
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

	return sysfs_emit(buf, "%d\n", pdev->tsm->state >= PCI_TSM_CONNECT);
}
static DEVICE_ATTR_RW(connect);

static bool pci_tsm_group_visible(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (pdev->tsm && pdev->tsm->state > PCI_TSM_IDLE)
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

static int pci_tsm_add(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held(&pci_tsm_rwsem);
	if (!tsm_ops)
		return 0;
	if (pci_tsm->state < PCI_TSM_INIT) {
		int rc = tsm_ops->add(pdev);

		if (rc)
			return rc;
	}
	pci_tsm->state = PCI_TSM_INIT;
	return sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
}

static void pci_tsm_del(struct pci_dev *pdev)
{
	struct pci_tsm *pci_tsm = pdev->tsm;

	lockdep_assert_held(&pci_tsm_rwsem);
	/* shutdown sysfs operations before tsm delete */
	scoped_guard(mutex, &pdev->tsm->exec_lock)
		pci_tsm->state = PCI_TSM_IDLE;
	sysfs_update_group(&pdev->dev.kobj, &pci_tsm_attr_group);
	tsm_ops->del(pdev);
}

int pci_tsm_register(const struct pci_tsm_ops *ops)
{
	struct pci_dev *pdev;
	unsigned long index;

	if (!ops)
		return 0;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (tsm_ops)
		return -EBUSY;
	tsm_ops = ops;
	xa_for_each(&pci_tsm_devs, index, pdev)
		pci_tsm_add(pdev);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_tsm_register);

void pci_tsm_unregister(const struct pci_tsm_ops *ops)
{
	struct pci_dev *pdev;
	unsigned long index;

	if (!ops)
		return;
	guard(rwsem_write)(&pci_tsm_rwsem);
	if (ops != tsm_ops)
		return;
	xa_for_each(&pci_tsm_devs, index, pdev)
		pci_tsm_del(pdev);
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

static unsigned long pci_tsm_devid(struct pci_dev *pdev)
{
	return FIELD_PREP(GENMASK(15, 0),
			  PCI_DEVID(pdev->bus->number, pdev->devfn)) |
	       FIELD_PREP(GENMASK(31, 16), pci_domain_nr(pdev->bus));
}

void pci_tsm_init(struct pci_dev *pdev)
{
	bool tee_cap;
	u16 ide_cap;
	int rc;

	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	tee_cap = pdev->devcap & PCI_EXP_DEVCAP_TEE;

	if (ide_cap || tee_cap)
		pci_dbg(pdev,
			"Device security capabailities detected (%s%s ), init TSM\n",
			ide_cap ? " ide" : "", tee_cap ? " tee" : "");
	else
		return;

	struct pci_tsm *pci_tsm __free(kfree) = kzalloc(sizeof(*pci_tsm), GFP_KERNEL);
	if (!pci_tsm)
		return;

	pci_tsm->ide_cap = ide_cap;
	mutex_init(&pci_tsm->exec_lock);

	pci_tsm->doe_mb = pci_find_doe_mailbox(pdev, PCI_VENDOR_ID_PCI_SIG,
					       PCI_DOE_PROTO_CMA);
	if (!pci_tsm->doe_mb)
		return;

	rc = xa_insert(&pci_tsm_devs, pci_tsm_devid(pdev), pdev, GFP_KERNEL);
	if (rc) {
		pci_dbg(pdev, "failed to register TSM capable device\n");
		return;
	}

	guard(rwsem_write)(&pci_tsm_rwsem);
	pdev->tsm = no_free_ptr(pci_tsm);
	pci_tsm_add(pdev);
}

void pci_tsm_destroy(struct pci_dev *pdev)
{
	guard(rwsem_write)(&pci_tsm_rwsem);
	pci_tsm_del(pdev);
	xa_erase(&pci_tsm_devs, pci_tsm_devid(pdev));
	kfree(pdev->tsm);
	pdev->tsm = NULL;
}
