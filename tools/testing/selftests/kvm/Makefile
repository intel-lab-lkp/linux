# SPDX-License-Identifier: GPL-2.0-only
top_srcdir = ../../../..

include $(top_srcdir)/scripts/subarch.include
ARCH 	?= $(SUBARCH)
SRCARCH := $(ARCH)

ifeq ($(ARCH),x86_64)
        SRCARCH := x86
endif

ifeq ($(SRCARCH),$(filter $(SRCARCH),arm64 s390 riscv x86))
include Makefile.kvm
else
# Empty targets for unsupported architectures
all:
clean:
endif
