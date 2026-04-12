# SPDX-License-Identifier: GPL-2.0
# Makefile for rv selftests

TEST_GEN_PROGS := tlob_helper tlob_uprobe_target

TEST_PROGS := \
	test_tlob.sh \

# TOOLS_INCLUDES is defined by ../lib.mk; provides -isystem to
# tools/include/uapi so that #include <linux/rv.h> resolves to the
# in-tree UAPI header without requiring make headers_install.
# Note: both must be added to the global variables, not as target-specific
# overrides, because lib.mk rewrites TEST_GEN_PROGS to $(OUTPUT)/name
# before per-target rules would be evaluated.
CFLAGS += $(TOOLS_INCLUDES)
LDLIBS += -lpthread

include ../lib.mk
