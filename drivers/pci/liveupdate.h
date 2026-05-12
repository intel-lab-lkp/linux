/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCI Live Update support (core API)
 *
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */
#ifndef DRIVERS_PCI_LIVEUPDATE_H
#define DRIVERS_PCI_LIVEUPDATE_H

#include <linux/pci.h>

#ifdef CONFIG_PCI_LIVEUPDATE
void pci_liveupdate_setup_device(struct pci_dev *dev);
void pci_liveupdate_cleanup_device(struct pci_dev *dev);
void pci_liveupdate_freeze(struct pci_dev *dev);
bool pci_liveupdate_inherit_buses(void);
void pci_liveupdate_init_acs(struct pci_dev *dev);
bool pci_liveupdate_inherit_acs(struct pci_dev *dev);
bool pci_liveupdate_inherit_ari(struct pci_dev *dev);

static inline bool pci_liveupdate_is_outgoing(struct pci_dev *dev)
{
	guard(read_lock)(&dev->liveupdate.lock);
	pci_WARN_ONCE(dev, !dev->liveupdate.frozen, "Preservation status is unstable!\n");
	return dev->liveupdate.outgoing;
}
#else
static inline void pci_liveupdate_setup_device(struct pci_dev *dev)
{
}

static inline void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
}

static inline void pci_liveupdate_freeze(struct pci_dev *dev);
{
}

static inline bool pci_liveupdate_inherit_buses(void)
{
	return false;
}

static inline void pci_liveupdate_init_acs(struct pci_dev *dev)
{
}

static inline bool pci_liveupdate_inherit_acs(struct pci_dev *dev)
{
	return false;
}

static inline bool pci_liveupdate_inherit_ari(struct pci_dev *dev)
{
	return false;
}

static inline bool pci_liveupdate_is_outgoing(struct pci_dev *dev)
{
	return false;
}
#endif

#endif /* DRIVERS_PCI_LIVEUPDATE_H */
