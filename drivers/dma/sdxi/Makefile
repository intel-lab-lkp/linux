# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_SDXI) += sdxi.o

sdxi-objs += \
	context.o     \
	device.o      \
	ring.o

sdxi-$(CONFIG_PCI_MSI) += pci.o

obj-$(CONFIG_SDXI_KUNIT_TEST) += \
	ring_kunit.o
