# SPDX-License-Identifier: GPL-2.0-or-later

CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES)

TEST_GEN_PROGS := fusectl_test
TEST_PROGS := symlink_test.sh
TEST_GEN_FILES := fuse_mnt symlink_fs

include ../../lib.mk

VAR_CFLAGS := $(shell pkg-config fuse3 --cflags 2>/dev/null)
ifeq ($(VAR_CFLAGS),)
VAR_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
endif

VAR_LDLIBS := $(shell pkg-config fuse3 --libs 2>/dev/null)
ifeq ($(VAR_LDLIBS),)
VAR_LDLIBS := -lfuse3 -pthread
endif

$(OUTPUT)/fuse_mnt: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/fuse_mnt: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/symlink_fs: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/symlink_fs: LDLIBS += $(VAR_LDLIBS)
