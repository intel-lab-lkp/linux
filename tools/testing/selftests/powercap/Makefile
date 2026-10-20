# SPDX-License-Identifier: GPL-2.0

# User-space test script
TEST_PROGS := powercap_hierarchy.sh

# Kernel module built as part of the test
TEST_GEN_FILES := powercap_hierarchy.ko

KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

.DEFAULT_GOAL := all

include ../lib.mk

.PHONY: modules

$(OUTPUT)/powercap_hierarchy.ko: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(OUTPUT) modules

.PHONY: clean_modules

clean: clean_modules

clean_modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(OUTPUT) clean
