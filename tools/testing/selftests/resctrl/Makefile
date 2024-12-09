# SPDX-License-Identifier: GPL-2.0
top_srcdir = ../../../..

CFLAGS = -g -Wall -O2 -D_FORTIFY_SOURCE=2 -I$(top_srcdir)/tools/include
CFLAGS += $(KHDR_INCLUDES)

TEST_GEN_PROGS := resctrl_tests

LOCAL_HDRS += $(wildcard *.h)

include ../lib.mk

$(OUTPUT)/resctrl_tests: $(wildcard *.c)
