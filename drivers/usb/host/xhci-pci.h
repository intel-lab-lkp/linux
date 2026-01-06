/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2019-2020 Linaro Limited */

#ifndef XHCI_PCI_H
#define XHCI_PCI_H

int xhci_pci_common_probe(struct pci_dev *dev, const struct pci_device_id *id);
void xhci_pci_remove(struct pci_dev *dev);
#if IS_REACHABLE(CONFIG_USB_XHCI_PCI)
void xhci_msix_set_handler_data(struct usb_hcd *hcd);
#else
static inline void xhci_msix_set_handler_data(struct usb_hcd *hcd) { }
#endif

#endif
