/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022, Google LLC.
 */

#ifndef SELFTEST_KVM_VFIO_UTIL_H
#define SELFTEST_KVM_VFIO_UTIL_H

#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include "kvm_util.h"
#include "test_util.h"

struct vfio_pci_dev {
	int fd;
	int group_fd;
	int container_fd;
};

struct vfio_pci_dev *__vfio_pci_init(const char *bdf, unsigned long iommu_type);
void vfio_pci_free(struct vfio_pci_dev *dev);

static inline struct vfio_pci_dev *vfio_pci_init(const char *bdf)
{
	return __vfio_pci_init(bdf, VFIO_TYPE1v2_IOMMU);
}

#define __vfio_ioctl(vfio_fd, cmd, arg)				\
({								\
	__kvm_ioctl(vfio_fd, cmd, arg);				\
})

#define vfio_ioctl(vfio_fd, cmd, arg)				\
({								\
	int ret = __vfio_ioctl(vfio_fd, cmd, arg);		\
								\
	TEST_ASSERT(!ret, __KVM_IOCTL_ERROR(#cmd, ret));	\
})

static inline uint32_t vfio_pci_get_nr_irqs(struct vfio_pci_dev *dev,
					    uint32_t irq_type)
{
	struct vfio_irq_info irq_info = {
		.argsz = sizeof(struct vfio_irq_info),
		.index = irq_type,
	};

	vfio_ioctl(dev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);

	TEST_ASSERT(irq_info.flags & VFIO_IRQ_INFO_EVENTFD,
		    "eventfd signalling unsupported by IRQ type '%u'", irq_type);
	return irq_info.count;
}

static inline uint32_t vfio_pci_get_nr_msi_irqs(struct vfio_pci_dev *dev)
{
	return vfio_pci_get_nr_irqs(dev, VFIO_PCI_MSI_IRQ_INDEX);
}

static inline uint32_t vfio_pci_get_nr_msix_irqs(struct vfio_pci_dev *dev)
{
	return vfio_pci_get_nr_irqs(dev, VFIO_PCI_MSIX_IRQ_INDEX);
}

static inline void __vfio_pci_irq_eventfd(struct vfio_pci_dev *dev, int eventfd,
					  uint32_t irq_type, uint32_t set)
{
	struct {
		struct vfio_irq_set vfio;
		uint32_t eventfd;
	} buffer = {};

	memset(&buffer, 0, sizeof(buffer));
	buffer.vfio.argsz = sizeof(buffer);
	buffer.vfio.flags = set | VFIO_IRQ_SET_ACTION_TRIGGER;
	buffer.vfio.index = irq_type;
	buffer.vfio.count = 1;
	buffer.eventfd = eventfd;

	vfio_ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &buffer.vfio);
}

static inline void vfio_pci_assign_irq_eventfd(struct vfio_pci_dev *dev,
					       int eventfd, uint32_t irq_type)
{
	__vfio_pci_irq_eventfd(dev, eventfd, irq_type, VFIO_IRQ_SET_DATA_EVENTFD);
}

static inline void vfio_pci_assign_msix(struct vfio_pci_dev *dev, int eventfd)
{
	vfio_pci_assign_irq_eventfd(dev, eventfd, VFIO_PCI_MSIX_IRQ_INDEX);
}

static inline void vfio_pci_release_irq_eventfds(struct vfio_pci_dev *dev,
						 uint32_t irq_type)
{
	struct vfio_irq_set vfio = {
		.argsz = sizeof(struct vfio_irq_set),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = irq_type,
		.count = 0,
	};

	vfio_ioctl(dev->fd, VFIO_DEVICE_SET_IRQS, &vfio);
}

static inline void vfio_pci_release_msix(struct vfio_pci_dev *dev)
{
	vfio_pci_release_irq_eventfds(dev, VFIO_PCI_MSIX_IRQ_INDEX);
}

static inline void vfio_pci_send_irq_eventfd(struct vfio_pci_dev *dev,
					     int eventfd, uint32_t irq_type)
{
	__vfio_pci_irq_eventfd(dev, eventfd, irq_type, VFIO_IRQ_SET_DATA_NONE);
}

static inline void vfio_pci_send_msix(struct vfio_pci_dev *dev, int eventfd)
{
	vfio_pci_send_irq_eventfd(dev, eventfd, VFIO_PCI_MSIX_IRQ_INDEX);
}

void *vfio_pci_map_bar(struct vfio_pci_dev *dev, unsigned int bar_idx,
		       uint64_t *size);

void vfio_pci_read_config_data(struct vfio_pci_dev *dev, size_t offset,
			       size_t size, void *data);

static inline uint16_t vfio_pci_config_read_u16(struct vfio_pci_dev *dev,
						size_t offset)
{
	uint16_t val;

	vfio_pci_read_config_data(dev, offset, sizeof(val), &val);
	return le16toh(val);
}

static inline uint16_t vfio_pci_get_vendor_id(struct vfio_pci_dev *dev)
{
	return vfio_pci_config_read_u16(dev, PCI_VENDOR_ID);
}

static inline uint16_t vfio_pci_get_device_id(struct vfio_pci_dev *dev)
{
	return vfio_pci_config_read_u16(dev, PCI_DEVICE_ID);
}

#endif /* SELFTEST_KVM_VFIO_UTIL_H */
