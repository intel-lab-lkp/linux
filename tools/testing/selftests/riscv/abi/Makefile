# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2021 ARM Limited
# Originally tools/testing/arm64/abi/Makefile

CFLAGS += -I$(top_srcdir)/tools/include

TEST_GEN_PROGS := ptrace_restart_syscall

include ../../lib.mk

$(OUTPUT)/ptrace_restart_syscall: ptrace_restart_syscall.c
	$(CC) -static -o$@ $(CFLAGS) $(LDFLAGS) $^
