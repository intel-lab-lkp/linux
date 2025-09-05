# SPDX-License-Identifier: GPL-2.0

ccflags-$(CONFIG_SDXI_DEBUG) += -DDEBUG

obj-$(CONFIG_SDXI) += sdxi.o

sdxi-objs += \
	context.o     \
	descriptor.o  \
	device.o      \
	dma.o         \
	enqueue.o     \
	error.o

sdxi-$(CONFIG_PCI_MSI) += pci.o

obj-$(CONFIG_SDXI_KUNIT_TEST) += descriptor_kunit.o
