# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wl,-no-as-needed -Wall $(KHDR_INCLUDES)

ifneq ($(LLVM),)
    CFLAGS += -Wno-address-of-packed-member
endif

LDLIBS += -lrt -lpthread -lm

TEST_GEN_PROGS = ftrace_test dyn_test perf_test abi_test

TEST_FILES := settings

include ../lib.mk
