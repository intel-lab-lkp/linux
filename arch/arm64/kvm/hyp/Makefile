# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Kernel-based Virtual Machine module, HYP part
#

hyp_includes := -I$(src)/include -I$(srctree)/arch/arm64/kvm
subdir-asflags-y := $(hyp_includes)
subdir-ccflags-y := $(hyp_includes)

obj-$(CONFIG_KVM) += vhe/ nvhe/ pgtable.o
