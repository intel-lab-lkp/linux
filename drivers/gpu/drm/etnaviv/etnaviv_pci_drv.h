/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ETNAVIV_PCI_DRV_H__
#define __ETNAVIV_PCI_DRV_H__

#ifdef CONFIG_DRM_ETNAVIV_PCI_DRIVER

int etnaviv_register_pci_driver(void);
void etnaviv_unregister_pci_driver(void);

#else

static inline int etnaviv_register_pci_driver(void) { return 0; }
static inline void etnaviv_unregister_pci_driver(void) { }

#endif

#endif
