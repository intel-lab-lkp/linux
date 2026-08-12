# SPDX-License-Identifier: GPL-2.0

KVM := ../../../../virt/kvm
include $(srctree)/virt/kvm/Makefile.kvm
include $(srctree)/arch/s390/kvm/gmap/Makefile
include $(src)/Makefile.gen

ccflags-y += -I $(src) -I $(objtree)/$(obj) -I$(srctree)/arch/s390/kvm/gmap -DKVM_S390_ARM64

kvm-arm64-obj := \
	arm.o \
	guest.o \
	handle_exit.o \
	inject_fault.o \
	mmu.o \
	mmio.o \
	reset.o \

kvm-arm64-obj += $(patsubst %.o,%-arm64.o,$(kvm-y))
kvm-arm64-obj += $(patsubst %.o,%-arm64.o,$(gmap-y))

targets += $(kvm-arm64-obj)

$(obj)/%-arm64.o: $(src)/%.c FORCE
	@mkdir -p $(dir $@)
	$(call if_changed_rule,cc_o_c)

ifeq ($(CONFIG_KVM_ARM64),m)

kvm-arm64-y = $(kvm-arm64-obj)

else ifeq ($(CONFIG_KVM_ARM64),y)

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

LDFLAGS_kvm-unnamespaced.o := -r --whole-archive
$(obj)/kvm-unnamespaced.o: $(addprefix $(obj)/,$(kvm-arm64-obj)) FORCE
	$(call if_changed_rule,ld_o_o)

quiet_cmd_nm_filter = NMFLTR  $@
	cmd_nm_filter = $(NM) -jU $< | sort -u > $@

$(obj)/kvm_symbol_list: $(obj)/kvm-unnamespaced.o FORCE
	$(call if_changed,nm_filter)

define rule_oc_o_o
	$(call cmd_and_savecmd,objcopy)
	$(call cmd,gen_objtooldep)
	$(call cmd,gen_symversions_o)
endef

OBJCOPYFLAGS_kvm-namespaced.o := -O default --localize-symbols=$(obj)/kvm_symbol_list
$(obj)/kvm-namespaced.o: $(obj)/kvm-unnamespaced.o $(obj)/kvm_symbol_list FORCE
	$(call if_changed_rule,oc_o_o)

kvm-arm64-y = kvm-namespaced.o

targets += kvm-unnamespaced.o kvm_symbol_list kvm-namespaced.o

endif

obj-$(CONFIG_KVM_ARM64) += kvm-arm64.o
