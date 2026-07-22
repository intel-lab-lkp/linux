# SPDX-License-Identifier: GPL-2.0
ARCH ?= $(shell uname -m 2>/dev/null || echo not)
override ARCH := $(shell echo $(ARCH) | sed -e s/i.86/x86/ -e s/x86_64/x86/)

# sched-cache tests an arch-independent prctl
TEST_PROGS := sched-cache

ifndef CROSS_COMPILE
ifeq ($(ARCH),x86)
TEST_PROGS += disable-tsc-ctxt-sw-stress-test disable-tsc-on-off-stress-test \
		disable-tsc-test set-anon-vma-name-test set-process-name
endif
endif

LDLIBS += -pthread

all: $(TEST_PROGS)

include ../lib.mk
