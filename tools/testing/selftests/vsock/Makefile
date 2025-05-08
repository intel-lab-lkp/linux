# SPDX-License-Identifier: GPL-2.0

CURDIR := $(abspath .)
TOOLSDIR := $(abspath ../../..)

$(OUTPUT)/vsock_test: $(TOOLSDIR)/testing/vsock/vsock_test
	install -m 755 $< $@

$(TOOLSDIR)/testing/vsock/vsock_test:
	$(MAKE) -C $(TOOLSDIR)/testing/vsock vsock_test

TEST_PROGS += vmtest.sh
EXTRA_CLEAN += vmtest.log
TEST_GEN_FILES := vsock_test

include ../lib.mk

