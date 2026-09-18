# SPDX-License-Identifier: GPL-2.0

KVM := ../../../../virt/kvm
KVM_CHECK_EXPORT_DIRS ?= $(srctree)/virt/kvm $(srctree)/arch/$(ARCH)/kvm/gmap $(src)
include $(srctree)/virt/kvm/Makefile.kvm
include $(srctree)/arch/s390/kvm/gmap/Makefile

ccflags-y += -I$(src) -I$(srctree)/arch/s390/kvm/gmap

kvm-y += s390.o intercept.o interrupt.o priv.o sigp.o
kvm-y += diag.o gaccess.o guestdbg.o vsie.o pv.o
kvm-y += $(gmap-y)

kvm-$(CONFIG_VFIO_PCI_ZDEV_KVM) += pci.o
obj-$(CONFIG_KVM) += kvm.o

# Fail the build if there is unexpected EXPORT_SYMBOL_GPL (or EXPORT_SYMBOL)
# usage in s390 KVM code. A symbol may only use EXPORT_SYMBOL_GPL if it is
# defined in exactly one of the two s390 KVM modules (kvm or kvm_arm64). A
# symbol defined in both would cause a conflict during linking if builtin is
# selected. Catch the issue early.
kvm_exports_allowed := kvm_s390_pv_cpu_is_protected \
		       kvm_arch_crypto_set_masks \
		       kvm_arch_crypto_clear_masks \
		       kvm_s390_gisc_register \
		       kvm_s390_gisc_unregister \
		       kvm_file_to_kvm_fn

$(eval $(call kvm_check_exports,EXPORT_SYMBOL_GPL))
$(eval $(call kvm_check_exports,EXPORT_SYMBOL))
