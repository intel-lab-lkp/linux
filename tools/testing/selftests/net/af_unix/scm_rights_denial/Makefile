# SPDX-License-Identifier: GPL-2.0
top_srcdir := ../../../../../..
include $(top_srcdir)/scripts/Makefile.compiler

cc-option = $(call __cc-option, $(CC),,$(1),$(2))

CFLAGS += $(KHDR_INCLUDES) -Wall $(call cc-option,-Wflex-array-member-not-at-end)

TEST_PROGS := scm_rights_denial.sh

TEST_GEN_FILES := sender receiver

include ../../../lib.mk
