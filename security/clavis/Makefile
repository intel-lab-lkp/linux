# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_SECURITY_CLAVIS) += clavis_keyring.o
ifeq ($(CONFIG_EFI),y)
obj-$(CONFIG_SECURITY_CLAVIS) += clavis_efi.o
endif
