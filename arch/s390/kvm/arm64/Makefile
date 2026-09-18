# SPDX-License-Identifier: GPL-2.0

KVM := ../../../../virt/kvm
KVM_DEV_NAME = kvm_arm64
KVM_DEV_MINOR = MISC_DYNAMIC_MINOR
KVM_CHECK_EXPORT_DIRS ?= $(srctree)/virt/kvm $(srctree)/arch/s390/kvm/gmap $(src)
include $(srctree)/virt/kvm/Makefile.kvm
include $(srctree)/arch/s390/kvm/gmap/Makefile
include $(src)/Makefile.gen

ccflags-y += -I $(src) -I $(objtree)/$(obj) -I$(srctree)/arch/s390/kvm/gmap -DKVM_S390_ARM64

kvm-arm64-obj := \
	arm.o \
	guest.o \
	handle_exit.o \
	inject_fault.o \
	mmio.o \
	mmu.o \
	reset.o \

kvm-arm64-obj += $(patsubst %.o,%-arm64.o,$(kvm-y))
kvm-arm64-obj += $(patsubst %.o,%-arm64.o,$(gmap-y))

targets += $(kvm-arm64-obj)

$(obj)/%-arm64.o: $(src)/%.c FORCE
	@mkdir -p $(dir $@)
	$(call if_changed_rule,cc_o_c)

ifeq ($(CONFIG_KVM),m)

kvm-arm64-y = $(kvm-arm64-obj)

else ifeq ($(CONFIG_KVM),y)

KVM_ARM64_GEN_DIR := $(objtree)/arch/${SRCARCH}/include/generated/asm
KVM_ARM64_MODNAME_H := $(KVM_ARM64_GEN_DIR)/kvm_arm64_modname.h
ccflags-y += -include $(KVM_ARM64_MODNAME_H)

targets += $(KVM_ARM64_MODNAME_H)

quiet_cmd_kvm_arm64_modname_h = GEN     $@
      cmd_kvm_arm64_modname_h = { \
	echo '/* Automatically generated; do not edit. */'; \
	echo '\#ifndef _KVM_ARM64_MODNAME_H'; \
	echo '\#define _KVM_ARM64_MODNAME_H'; \
	echo '\#undef KBUILD_MODNAME'; \
	echo '\#define KBUILD_MODNAME "kvm_arm64"'; \
	echo '\#endif /* _KVM_ARM64_MODNAME_H */'; \
	} > $@

$(addprefix $(obj)/,$(kvm-arm64-obj)): $(KVM_ARM64_MODNAME_H)

$(KVM_ARM64_MODNAME_H):
	@mkdir -p $(KVM_ARM64_GEN_DIR)
	$(call cmd,kvm_arm64_modname_h)

prereq-o-cmd = $(foreach o, $(filter %.o, $^), $(dir $(o)).$(notdir $(o)).cmd)
cmd_gen_symversions_o = \
	grep --no-filename "^\#SYMVER" $(prereq-o-cmd) >> $(dot-target).cmd || true

define rule_ld_o_o
	$(call cmd_and_savecmd,ld)
	$(call cmd,gen_symversions_o)
endef

LDFLAGS_kvm-unnamespaced.o := -r
$(obj)/kvm-unnamespaced.o: $(addprefix $(obj)/,$(kvm-arm64-obj)) FORCE
	$(call if_changed_rule,ld_o_o)

quiet_cmd_nm_filter = NMFLTR  $@
	cmd_nm_filter = $(NM) --defined-only --format=just-symbols $< | sort -u > $@

$(obj)/kvm_symbol_list: $(obj)/kvm-unnamespaced.o FORCE
	$(call if_changed,nm_filter)

define rule_oc_o_o
	$(call cmd_and_savecmd,objcopy)
	$(call cmd,gen_objtooldep)
	$(call cmd,gen_symversions_o)
endef

OBJCOPYFLAGS_kvm-namespaced.o := -O $(LD_BFD) --localize-symbols=$(obj)/kvm_symbol_list
$(obj)/kvm-namespaced.o: $(obj)/kvm-unnamespaced.o $(obj)/kvm_symbol_list FORCE
	$(call if_changed_rule,oc_o_o)

kvm-arm64-y = kvm-namespaced.o

targets += kvm-unnamespaced.o kvm_symbol_list kvm-namespaced.o

endif

obj-$(CONFIG_KVM) += kvm-arm64.o

# Fail the build if there is unexpected EXPORT_SYMBOL_GPL (or EXPORT_SYMBOL)
# usage in arm64 KVM code. A symbol may only use EXPORT_SYMBOL_GPL if it is
# defined in exactly one of the two s390 KVM modules (kvm or kvm_arm64). A
# symbol defined in both would cause a conflict during linking if builtin is
# selected. Catch the issue early.

# This "symbol" is not exported by arm on s390 but still in the source code and the
# export check scans unexpanded source code.
kvm_exports_allowed := kvm_file_to_kvm_fn

$(eval $(call kvm_check_exports,EXPORT_SYMBOL_GPL))
$(eval $(call kvm_check_exports,EXPORT_SYMBOL))
