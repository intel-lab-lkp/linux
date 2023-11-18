# SPDX-License-Identifier: GPL-2.0
include ../../../../scripts/Makefile.include

SUBDIRS := bpf

all:
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) -C $$SUBDIR;\
	done;

install:
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) INSTALL_PATH=$(INSTALL_PATH)/$$SUBDIR -C $$SUBDIR install;\
	done;

clean:
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) -C $$SUBDIR clean;\
	done;

.PHONY: all install clean
