// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright Red Hat

#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include "ice_adapter.h"

static DEFINE_MUTEX(ice_adapters_lock);
static DEFINE_XARRAY(ice_adapters);

static unsigned long ice_adapter_index(const struct pci_dev *pdev)
{
	unsigned int domain = pci_domain_nr(pdev->bus);

	WARN_ON((unsigned long)domain >> (BITS_PER_LONG - 13));
	return ((unsigned long)domain << 13) |
	       ((unsigned long)pdev->bus->number << 5) |
	       PCI_SLOT(pdev->devfn);
}

struct ice_adapter *ice_adapter_get(const struct pci_dev *pdev)
{
	unsigned long index = ice_adapter_index(pdev);
	struct ice_adapter *a;

	guard(mutex)(&ice_adapters_lock);

	a = xa_load(&ice_adapters, index);
	if (a) {
		refcount_inc(&a->refcount);
		return a;
	}

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return NULL;

	refcount_set(&a->refcount, 1);

	if (xa_is_err(xa_store(&ice_adapters, index, a, GFP_KERNEL))) {
		kfree(a);
		return NULL;
	}

	return a;
}

void ice_adapter_put(const struct pci_dev *pdev)
{
	unsigned long index = ice_adapter_index(pdev);
	struct ice_adapter *a;

	guard(mutex)(&ice_adapters_lock);

	a = xa_load(&ice_adapters, index);
	if (WARN_ON(!a))
		return;

	if (!refcount_dec_and_test(&a->refcount))
		return;

	WARN_ON(xa_erase(&ice_adapters, index) != a);
	kfree(a);
}
