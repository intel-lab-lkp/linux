# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_SECURITY_CLAVIS) += clavis_keyring.o
obj-$(CONFIG_SECURITY_CLAVIS) += clavis.o
obj-$(CONFIG_SECURITY_CLAVIS) += clavis_builtin_acl.o

ifeq ($(CONFIG_SECURITY_CLAVIS_ACL_LIST),)
quiet_cmd_make_builtin_acl = GEN     $@
      cmd_make_builtin_acl = \
	echo > $@
else
quiet_cmd_make_builtin_acl = GEN     $@
      cmd_make_builtin_acl = \
	sed 's/^[ \t]*//; s/.*/"&",/' $< | tr '[:upper:]' '[:lower:]' > $@
endif

$(obj)/builtin_acl: $(CONFIG_SECURITY_CLAVIS_ACL_LIST) FORCE
	$(call if_changed,make_builtin_acl)

$(obj)/clavis_builtin_acl.o: $(obj)/builtin_acl

ifeq ($(CONFIG_EFI),y)
obj-$(CONFIG_SECURITY_CLAVIS) += clavis_efi.o
endif
