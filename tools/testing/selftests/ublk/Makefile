# SPDX-License-Identifier: GPL-2.0

CFLAGS += -O3 -Wl,-no-as-needed -Wall -I $(top_srcdir)
LDLIBS += -lpthread -lm -luring

TEST_PROGS := test_null_01.sh

# Order correspond to 'make run_tests' order
TEST_GEN_PROGS_EXTENDED = kublk

include ../lib.mk
