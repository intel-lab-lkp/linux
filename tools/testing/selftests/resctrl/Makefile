# SPDX-License-Identifier: GPL-2.0

CFLAGS = -g -Wall -O2 -D_FORTIFY_SOURCE=2
CFLAGS += $(KHDR_INCLUDES)

TEST_GEN_PROGS := resctrl_tests fastcat

LOCAL_HDRS += $(wildcard *.h)

include ../lib.mk

$(OUTPUT)/resctrl_tests: cache.c cat_test.c cmt_test.c fill_buf.c mba_test.c mbm_test.c resctrlfs.c resctrl_tests.c resctrl_val.c

$(OUTPUT)/fastcat: fastcat.c
