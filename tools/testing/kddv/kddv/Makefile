# SPDX-License-Identifier: GPL-2.0
include ../../../scripts/Makefile.include

INSTALL ?= install

all:
	@for SUBDIR in $(SUBDIRS); do		\
		$(MAKE) -C $$SUBDIR;		\
	done;

install:
	$(INSTALL) -m 0755 -d $(INSTALL_PATH)
	$(INSTALL) __init__.py $(INSTALL_PATH)
	cp -rf core/ $(INSTALL_PATH)
	cp -rf tests/ $(INSTALL_PATH)

clean:
	@for SUBDIR in $(SUBDIRS); do		\
		$(MAKE) -C $$SUBDIR clean;	\
	done;

.PHONY: all install clean
