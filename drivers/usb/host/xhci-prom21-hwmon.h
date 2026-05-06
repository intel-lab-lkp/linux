/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */

#ifndef XHCI_PROM21_HWMON_H
#define XHCI_PROM21_HWMON_H

#define PCI_DEVICE_ID_AMD_PROM21_XHCI 0x43fd

struct pci_dev;
struct xhci_hcd;

#if IS_ENABLED(CONFIG_USB_XHCI_PCI_PROM21_HWMON)
void xhci_prom21_hwmon_init(struct xhci_hcd *xhci, struct pci_dev *pdev);
void xhci_try_prom21_hwmon_invalidate(struct pci_dev *pdev);
#else
static inline void xhci_prom21_hwmon_init(struct xhci_hcd *xhci,
					  struct pci_dev *pdev)
{
}

static inline void xhci_try_prom21_hwmon_invalidate(struct pci_dev *pdev)
{
}
#endif

#endif
