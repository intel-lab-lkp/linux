# SPDX-License-Identifier: GPL-2.0

GMAP ?= ../gmap

# Enable s390-specific guest management features (storage keys and CMMA)
KVM_MANAGES_S390_GUEST ?= y

kvm-y += $(GMAP)/dat.o $(GMAP)/gmap.o $(GMAP)/faultin.o $(GMAP)/mmu.o

kvm-$(KVM_MANAGES_S390_GUEST) += $(GMAP)/sk.o $(GMAP)/cmma.o
