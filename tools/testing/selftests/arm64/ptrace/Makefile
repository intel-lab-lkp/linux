# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES)

TEST_GEN_PROGS := sysemu_singlestep_no_exit_stop \
		  sysemu_singlestep_intercept \
		  single_step_signal_handler \
		  single_step_syscall_stop

include ../../lib.mk
