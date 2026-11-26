# SPDX-License-Identifier: GPL-2.0
#
# First run: make -C ../../../.. headers_install

CFLAGS += -Wall -O2 $(KHDR_INCLUDES)
LOCAL_HDRS += common.h

TEST_GEN_PROGS := lsm_get_self_attr_test lsm_list_modules_test \
		  lsm_set_self_attr_test

# lockdown_test changes the machine's lockdown level, so do not run it
# by default.
TEST_GEN_PROGS_EXTENDED := lockdown_test

include ../lib.mk

$(OUTPUT)/lsm_get_self_attr_test: lsm_get_self_attr_test.c common.c
$(OUTPUT)/lsm_set_self_attr_test: lsm_set_self_attr_test.c common.c
$(OUTPUT)/lsm_list_modules_test: lsm_list_modules_test.c common.c
$(OUTPUT)/lockdown_test: lockdown_test.c common.c

run_lockdown_test: all
	$(call RUN_TESTS, lockdown_test)

EXTRA_CLEAN = $(OUTPUT)/common.o
