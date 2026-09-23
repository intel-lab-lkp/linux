# SPDX-License-Identifier: GPL-2.0

# User-space test script
TEST_PROGS := powercap_hierarchy.sh

# Kernel module built as part of the test
TEST_FILES := powercap_hierarchy.ko

KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

include ../lib.mk
