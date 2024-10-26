# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -g -isystem $(top_srcdir)/tools/include $(KHDR_INCLUDES) -pthread -Wall

TEST_GEN_PROGS := pidfd_test pidfd_fdinfo_test pidfd_open_test \
	pidfd_poll_test pidfd_wait pidfd_getfd_test pidfd_setns_test

include ../lib.mk
