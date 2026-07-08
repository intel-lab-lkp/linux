# SPDX-License-Identifier: GPL-2.0-or-later

CFLAGS += -Wall -O2 -g $(KHDR_INCLUDES)

TEST_GEN_PROGS := fusectl_test
TEST_PROGS := symlink_test.sh acl_test.sh
TEST_GEN_FILES := fuse_mnt symlink_fs acl_fs setgetacl

include ../../lib.mk

VAR_CFLAGS := $(shell pkg-config fuse3 --cflags 2>/dev/null)
ifeq ($(VAR_CFLAGS),)
VAR_CFLAGS := -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
endif

VAR_LDLIBS := $(shell pkg-config fuse3 --libs 2>/dev/null)
ifeq ($(VAR_LDLIBS),)
VAR_LDLIBS := -lfuse3 -pthread
endif

ACL_CFLAGS := $(shell pkg-config libacl --cflags 2>/dev/null)
ACL_LDLIBS := $(shell pkg-config libacl --libs 2>/dev/null)

$(OUTPUT)/fuse_mnt: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/fuse_mnt: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/symlink_fs: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/symlink_fs: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/acl_fs: CFLAGS += $(VAR_CFLAGS)
$(OUTPUT)/acl_fs: LDLIBS += $(VAR_LDLIBS)

$(OUTPUT)/setgetacl: CFLAGS += $(ACL_CFLAGS)
$(OUTPUT)/setgetacl: LDLIBS += $(ACL_LDLIBS)
