# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wl,-no-as-needed -Wall $(KHDR_INCLUDES)
LDFLAGS += -lpthread
LDLIBS += -lcap

TEST_GEN_PROGS := seccomp_bpf seccomp_benchmark seccomp_pin_args
include ../lib.mk
