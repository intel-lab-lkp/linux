# SPDX-License-Identifier: GPL-2.0
include ../../../scripts/Makefile.include

INSTALL ?= install
SUBDIRS := data

all:
	@for SUBDIR in $(SUBDIRS); do		\
		$(MAKE) -C $$SUBDIR;		\
	done;

install:
	$(INSTALL) -m 0755 -d $(INSTALL_PATH)
	$(INSTALL) __init__.py $(INSTALL_PATH)
	cp -rf core/ $(INSTALL_PATH)
	cp -rf cmds/ $(INSTALL_PATH)
	cp -rf tests/ $(INSTALL_PATH)

	@for SUBDIR in $(SUBDIRS); do		\
		$(MAKE) INSTALL_PATH=$(INSTALL_PATH)/$$SUBDIR -C $$SUBDIR install; \
	done;

clean:
	@for SUBDIR in $(SUBDIRS); do		\
		$(MAKE) -C $$SUBDIR clean;	\
	done;

.PHONY: all install clean
