# SPDX-License-Identifier: GPL-2.0

TEST_PROGS := verificationtest-ktap
TEST_FILES := test.d settings
EXTRA_CLEAN := $(OUTPUT)/logs/*

# Subdirectories that provide binaries used by the test runner.
# Each entry must contain a Makefile that accepts OUTDIR= and
# deposits its binaries there.
BUILD_SUBDIRS := test.d/tlob

include ../lib.mk

all: $(patsubst %,_build_%,$(BUILD_SUBDIRS))

clean: $(patsubst %,_clean_%,$(BUILD_SUBDIRS))

.PHONY: $(patsubst %,_build_%,$(BUILD_SUBDIRS)) \
        $(patsubst %,_clean_%,$(BUILD_SUBDIRS))

$(patsubst %,_build_%,$(BUILD_SUBDIRS)): _build_%:
	$(MAKE) -C $* OUTDIR="$(OUTPUT)" TOOLS_INCLUDES="$(TOOLS_INCLUDES)"

$(patsubst %,_clean_%,$(BUILD_SUBDIRS)): _clean_%:
	$(MAKE) -C $* OUTDIR="$(OUTPUT)" clean
