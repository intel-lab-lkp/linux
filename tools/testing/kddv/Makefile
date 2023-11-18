# SPDX-License-Identifier: GPL-2.0
include ../../scripts/Makefile.include

SUBDIRS := kddv

all:
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) -C $$SUBDIR;\
	done;

install:
ifdef INSTALL_PATH
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) INSTALL_PATH=$(INSTALL_PATH)/$$SUBDIR -C $$SUBDIR install;\
	done;
else
	$(error Error: set INSTALL_PATH to use install)
endif

clean:
	@for SUBDIR in $(SUBDIRS); do \
		$(MAKE) -C $$SUBDIR clean;\
	done;

.PHONY: all install clean
