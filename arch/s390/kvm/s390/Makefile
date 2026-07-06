# SPDX-License-Identifier: GPL-2.0
# Makefile for kernel virtual machines on s390
#
# Copyright IBM Corp. 2008

KVM := ../../../../virt/kvm
include $(srctree)/virt/kvm/Makefile.kvm
include $(srctree)/arch/s390/kvm/gmap/Makefile

ccflags-y := -I$(src) -I$(srctree)/arch/s390/kvm/gmap

kvm-y += s390.o intercept.o interrupt.o priv.o sigp.o
kvm-y += diag.o gaccess.o guestdbg.o vsie.o pv.o

kvm-$(CONFIG_VFIO_PCI_ZDEV_KVM) += pci.o

obj-$(CONFIG_KVM_S390) += kvm.o
