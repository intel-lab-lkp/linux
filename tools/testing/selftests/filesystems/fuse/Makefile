# SPDX-License-Identifier: GPL-2.0-or-later

CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES)

TEST_GEN_PROGS := fusectl_test
TEST_GEN_PROGS += write_extend_eof_test
TEST_GEN_FILES := fuse_mnt

# These tests require libfuse3; add them only when the library is present.
FUSE3_CFLAGS := $(shell pkg-config fuse3 --cflags 2>/dev/null)
FUSE3_LDLIBS := $(shell pkg-config fuse3 --libs 2>/dev/null)
ifneq ($(FUSE3_CFLAGS),)
TEST_GEN_PROGS += fuse_acl_cache_test
endif

include ../../lib.mk

$(OUTPUT)/write_extend_eof_test: LDLIBS += -lpthread

VAR_CFLAGS := $(shell pkg-config fuse --cflags 2>/dev/null)
ifeq ($(VAR_CFLAGS),)
VAR_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
endif

VAR_LDLIBS := $(shell pkg-config fuse --libs 2>/dev/null)
ifeq ($(VAR_LDLIBS),)
VAR_LDLIBS := -lfuse -pthread
endif

$(OUTPUT)/fuse_mnt: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/fuse_mnt: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/fuse_acl_cache_test: CFLAGS += $(FUSE3_CFLAGS)
$(OUTPUT)/fuse_acl_cache_test: LDLIBS += $(FUSE3_LDLIBS)
