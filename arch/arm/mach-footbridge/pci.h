/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __FOOTBRIDGE_PCI_H
#define __FOOTBRIDGE_PCI_H

/* PCI controller-related definitions for the DC21285 Footbridge chip */
extern struct pci_ops dc21285_ops;
extern int dc21285_setup(int nr, struct pci_sys_data *sys);
extern void dc21285_preinit(void);
extern void dc21285_postinit(void);

#endif /* __FOOTBRIDGE_PCI_H */
