# SPDX-License-Identifier: GPL-2.0

TEST_GEN_PROGS = get_unit get_limits
TEST_GEN_PROGS += get_set_min get_set_max
TEST_GEN_PROGS += get_set_min_forked get_set_max_forked
TEST_GEN_PROGS += set_min_forked set_max_forked
TEST_GEN_PROGS += set_multiple_mins set_multiple_maxs
TEST_GEN_PROGS += set_same_multiple_mins set_same_multiple_maxs

include ../lib.mk

TOOLSDIR := $(top_srcdir)/tools
TOOLSINCDIR := $(TOOLSDIR)/include
APIDIR := $(TOOLSINCDIR)/uapi

CFLAGS += -Wall -O2 -I$(APIDIR)
