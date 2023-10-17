/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VIRTIO_PCI_ADMIN_H
#define _LINUX_VIRTIO_PCI_ADMIN_H

#include <linux/types.h>
#include <linux/pci.h>

int virtio_pci_admin_list_use(struct pci_dev *pdev, u8 *buf, int buf_size);
int virtio_pci_admin_list_query(struct pci_dev *pdev, u8 *buf, int buf_size);
int virtio_pci_admin_legacy_io_write(struct pci_dev *pdev, u16 opcode,
				     u8 offset, u8 size, u8 *buf);
int virtio_pci_admin_legacy_io_read(struct pci_dev *pdev, u16 opcode,
				    u8 offset, u8 size, u8 *buf);
int virtio_pci_admin_legacy_io_notify_info(struct pci_dev *pdev,
					   u8 req_bar_flags, u8 *bar,
					   u64 *bar_offset);

#endif /* _LINUX_VIRTIO_PCI_ADMIN_H */
