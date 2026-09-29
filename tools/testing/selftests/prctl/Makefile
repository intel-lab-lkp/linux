# SPDX-License-Identifier: GPL-2.0
ARCH ?= $(shell uname -m 2>/dev/null || echo not)
override ARCH := $(shell echo $(ARCH) | sed -e s/i.86/x86/ -e s/x86_64/x86/)

TEST_GEN_PROGS := cap-bset-drop-test
LDLIBS += -lpthread

# The tests below are x86-only and embed x86 assembly, so they can only be
# built when not cross-compiling.
ifndef CROSS_COMPILE
ifeq ($(ARCH),x86)
TEST_PROGS := disable-tsc-ctxt-sw-stress-test disable-tsc-on-off-stress-test \
		disable-tsc-test set-anon-vma-name-test set-process-name
all: $(TEST_PROGS)
endif
endif

include ../lib.mk
