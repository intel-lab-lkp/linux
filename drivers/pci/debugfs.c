// SPDX-License-Identifier: GPL-2.0
/*
 * Per-device debugfs directories for the PCI core
 *
 * PCI features that want to export debug information create their files in
 * the directory returned by pci_dev_debugfs_dir().  The directory itself is
 * created on first use and is torn down, together with everything below it,
 * when the device is destroyed.
 */
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/pci.h>

#include "pci.h"

/* Serializes creation of the root and of the per-device directories */
static DEFINE_MUTEX(pci_debugfs_lock);
static struct dentry *pci_debugfs_root;

/**
 * pci_dev_debugfs_dir - Return the debugfs directory of a PCI device
 * @dev: PCI device
 *
 * Return the /sys/kernel/debug/pci/<device> directory of @dev, creating it
 * and the PCI root directory if they do not exist yet.  Devices that have no
 * debugfs content never get a directory.
 *
 * Return: the dentry of the directory, or an ERR_PTR() on failure.  The
 * return value only needs to be checked with IS_ERR() if the caller cares;
 * it can be passed to debugfs_create_*() as is.
 */
struct dentry *pci_dev_debugfs_dir(struct pci_dev *dev)
{
	guard(mutex)(&pci_debugfs_lock);

	if (!pci_debugfs_root)
		pci_debugfs_root = debugfs_create_dir("pci", NULL);

	if (!dev->debugfs)
		dev->debugfs = debugfs_create_dir(pci_name(dev),
						  pci_debugfs_root);

	return dev->debugfs;
}

/**
 * pci_dev_debugfs_remove - Remove the debugfs directory of a PCI device
 * @dev: PCI device
 *
 * Remove the device's debugfs directory and everything below it.  Returns
 * once no debugfs operation on those files is running any more, so the
 * owners of the files can drop the data the files refer to afterwards.
 */
void pci_dev_debugfs_remove(struct pci_dev *dev)
{
	guard(mutex)(&pci_debugfs_lock);

	debugfs_remove_recursive(dev->debugfs);
	dev->debugfs = NULL;
}
