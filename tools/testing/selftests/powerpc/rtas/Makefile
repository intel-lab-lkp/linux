# SPDX-License-Identifier: GPL-2.0
# RTAS tests

TEST_GEN_PROGS := rtas_rtc_test
TEST_GEN_PROGS += nvram_test
TEST_GEN_PROGS += rtas_proc_test
TEST_GEN_PROGS += fadump_test
TEST_GEN_PROGS += rtas_flash_test

top_srcdir = ../../../../..
include ../../lib.mk

$(TEST_GEN_PROGS): utils.h

CFLAGS += -I$(top_srcdir)/tools/testing/selftests/powerpc/include
CFLAGS += -Wall -Werror
