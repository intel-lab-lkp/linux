# SPDX-License-Identifier: GPL-2.0
CFLAGS_REMOVE_core.o	= -pg
KASAN_SANITIZE_core.o	:= n
CFLAGS_core.o		+= -fno-stack-protector

obj-y += core.o

# clang allows -fPIE with mcmodel=kernel; gcc currently does not.
ifdef CONFIG_CC_IS_CLANG
# Enforce PC-relative addressing for SEV/SME global variables.
CFLAGS_core.o		+= -fPIE
# Disable relocation relaxation in case the link is not PIE.
CFLAGS_core.o 		+= $(call cc-option,-Wa$(comma)-mrelax-relocations=no)
# Avoid unnecessary GOT overhead in PC-relative addressing.
CFLAGS_core.o 		+= -include $(srctree)/include/linux/hidden.h
endif

obj-$(CONFIG_INTEL_TDX_GUEST)	+= tdx/
