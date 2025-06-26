# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -g $(KHDR_INCLUDES)
LDLIBS += -lpthread

TEST_GEN_PROGS := membarrier_test_single_thread \
		membarrier_test_multi_thread \
		membarrier_test_expedited

include ../lib.mk
