// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright Red Hat

#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include "ice_adapter.h"

static DEFINE_XARRAY(ice_adapters);

static unsigned long ice_adapter_index(const struct pci_dev *pdev)
{
	unsigned int domain = pci_domain_nr(pdev->bus);

	WARN_ON((unsigned long)domain >> (BITS_PER_LONG - 13));
	return ((unsigned long)domain << 13) |
	       ((unsigned long)pdev->bus->number << 5) |
	       PCI_SLOT(pdev->devfn);
}

static struct ice_adapter *ice_adapter_new(void)
{
	struct ice_adapter *a;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return NULL;

	refcount_set(&a->refcount, 1);

	return a;
}

static void ice_adapter_free(struct ice_adapter *a)
{
	kfree(a);
}

DEFINE_FREE(ice_adapter_free, struct ice_adapter*, if (_T) ice_adapter_free(_T))

struct ice_adapter *ice_adapter_get(const struct pci_dev *pdev)
{
	struct ice_adapter *ret, __free(ice_adapter_free) *a = NULL;
	unsigned long index = ice_adapter_index(pdev);

	a = ice_adapter_new();
	if (!a)
		return NULL;

	xa_lock(&ice_adapters);
	ret = __xa_cmpxchg(&ice_adapters, index, NULL, a, GFP_KERNEL);
	if (xa_is_err(ret)) {
		ret = NULL;
		goto unlock;
	}
	if (ret) {
		refcount_inc(&ret->refcount);
		goto unlock;
	}
	ret = no_free_ptr(a);
unlock:
	xa_unlock(&ice_adapters);
	return ret;
}

void ice_adapter_put(const struct pci_dev *pdev)
{
	unsigned long index = ice_adapter_index(pdev);
	struct ice_adapter *a;

	xa_lock(&ice_adapters);
	a = xa_load(&ice_adapters, index);
	if (WARN_ON(!a))
		goto unlock;

	if (!refcount_dec_and_test(&a->refcount))
		goto unlock;

	WARN_ON(__xa_erase(&ice_adapters, index) != a);
	ice_adapter_free(a);
unlock:
	xa_unlock(&ice_adapters);
}
