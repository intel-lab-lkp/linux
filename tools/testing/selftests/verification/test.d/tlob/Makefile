# SPDX-License-Identifier: GPL-2.0
# Builds tlob selftest helper binaries in the directory of this Makefile.
#
# Invoked by ../../Makefile via BUILD_SUBDIRS; outputs tlob_sym and
# tlob_target alongside the .tc scripts so they are self-contained.

CFLAGS += $(TOOLS_INCLUDES)

# For standalone execution via vng
FTRACETEST := ../../../ftrace/ftracetest
LOGDIR ?= ../../logs

.PHONY: all
all: tlob_sym tlob_target

tlob_sym: tlob_sym.c
	$(CC) $(CFLAGS) -o $@ $<

tlob_target: tlob_target.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: run_tests
run_tests: all
	@./run_tlob_tests.sh

.PHONY: clean
clean:
	$(RM) tlob_sym tlob_target
