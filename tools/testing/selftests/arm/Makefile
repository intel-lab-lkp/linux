# SPDX-License-Identifier: GPL-2.0

# When ARCH not overridden for crosscompiling, lookup machine
ARCH ?= $(shell uname -m 2>/dev/null || echo not)

ifneq (,$(filter $(ARCH),aarch64 arm64 arm armv7l armv8l))
ARM_SUBTARGETS ?= mm signal elf
else
ARM_SUBTARGETS :=
endif

CFLAGS := -Wall -O2 -g -static

# A proper top_srcdir is needed by KSFT(lib.mk)
top_srcdir = $(realpath ../../../../)

# Additional include paths needed by kselftest.h and local headers
CFLAGS += -I$(top_srcdir)/tools/testing/selftests/

CFLAGS += -I$(top_srcdir)/tools/include

export CFLAGS
export top_srcdir

all:
	@for DIR in $(ARM_SUBTARGETS); do				\
		BUILD_TARGET=$(OUTPUT)/$$DIR;			\
		mkdir -p $$BUILD_TARGET;			\
		make OUTPUT=$$BUILD_TARGET -C $$DIR $@;		\
	done

install: all
	@for DIR in $(ARM_SUBTARGETS); do				\
		BUILD_TARGET=$(OUTPUT)/$$DIR;			\
		make OUTPUT=$$BUILD_TARGET -C $$DIR $@;		\
	done

run_tests: all
	@for DIR in $(ARM_SUBTARGETS); do				\
		BUILD_TARGET=$(OUTPUT)/$$DIR;			\
		make OUTPUT=$$BUILD_TARGET -C $$DIR $@;		\
	done

# Avoid any output on non arm on emit_tests
emit_tests:
	@for DIR in $(ARM_SUBTARGETS); do				\
		BUILD_TARGET=$(OUTPUT)/$$DIR;			\
		make OUTPUT=$$BUILD_TARGET -C $$DIR $@;		\
	done

clean:
	@for DIR in $(ARM_SUBTARGETS); do				\
		BUILD_TARGET=$(OUTPUT)/$$DIR;			\
		make OUTPUT=$$BUILD_TARGET -C $$DIR $@;		\
	done

.PHONY: all clean install run_tests emit_tests
