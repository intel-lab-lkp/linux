# SPDX-License-Identifier: GPL-2.0

TEST_PROGS := verificationtest-ktap
TEST_FILES := test.d settings
EXTRA_CLEAN := $(OUTPUT)/logs/*

# Subdirectories that provide helper binaries for the test runner.
# Each entry must contain a Makefile that accepts OUTDIR= and deposits
# its binaries there; verificationtest-ktap adds OUTDIR to PATH so
# the ftracetest require-checks resolve the binaries by name.
MONITOR_SUBDIRS := tlob

include ../lib.mk

# Build and clean each monitor subdirectory.
all: $(patsubst %,_build_%,$(MONITOR_SUBDIRS))

clean: $(patsubst %,_clean_%,$(MONITOR_SUBDIRS))

.PHONY: $(patsubst %,_build_%,$(MONITOR_SUBDIRS)) \
        $(patsubst %,_clean_%,$(MONITOR_SUBDIRS))

$(patsubst %,_build_%,$(MONITOR_SUBDIRS)): _build_%:
	$(MAKE) -C $* OUTDIR="$(OUTPUT)" TOOLS_INCLUDES="$(TOOLS_INCLUDES)"

$(patsubst %,_clean_%,$(MONITOR_SUBDIRS)): _clean_%:
	$(MAKE) -C $* OUTDIR="$(OUTPUT)" clean
