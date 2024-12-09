# SPDX-License-Identifier: GPL-2.0

CC=gcc
CFLAGS += -std=gnu11 -O2 -W -Wall -Wextra -Wno-unused-parameter -Wshadow \
	-I../lib/ -idirafter $(UAPI_PATH)
ifeq ("$(DEBUG)","1")
  CFLAGS += -g -fsanitize=address -fsanitize=leak -static-libasan
endif

INSTALL	    ?= install
prefix      ?= /usr
datarootdir ?= $(prefix)/share
docdir      ?= $(datarootdir)/doc

include ../Makefile.deps

YNL_GEN_ARG_ethtool:=--user-header linux/ethtool_netlink.h \
	--exclude-op stats-get

TOOL:=../pyynl/ynl_gen_c.py
TOOL_RST:=../pyynl/ynl_gen_rst.py

GENS_PATHS=$(shell grep -nrI --files-without-match \
		'protocol: netlink' \
		../../../../Documentation/netlink/specs/)
GENS=$(patsubst ../../../../Documentation/netlink/specs/%.yaml,%,${GENS_PATHS})
SRCS=$(patsubst %,%-user.c,${GENS})
HDRS=$(patsubst %,%-user.h,${GENS})
OBJS=$(patsubst %,%-user.o,${GENS})

SPECS_PATHS=$(wildcard ../../../../Documentation/netlink/specs/*.yaml)
SPECS=$(patsubst ../../../../Documentation/netlink/specs/%.yaml,%,${SPECS_PATHS})
RSTS=$(patsubst %,%.rst,${SPECS})

all: protos.a $(HDRS) $(SRCS) $(KHDRS) $(KSRCS) $(UAPI) $(RSTS)

protos.a: $(OBJS)
	@echo -e "\tAR $@"
	@ar rcs $@ $(OBJS)

%-user.h: ../../../../Documentation/netlink/specs/%.yaml $(TOOL)
	@echo -e "\tGEN $@"
	@$(TOOL) --mode user --header --spec $< -o $@ $(YNL_GEN_ARG_$*)

%-user.c: ../../../../Documentation/netlink/specs/%.yaml $(TOOL)
	@echo -e "\tGEN $@"
	@$(TOOL) --mode user --source --spec $< -o $@ $(YNL_GEN_ARG_$*)

%-user.o: %-user.c %-user.h
	@echo -e "\tCC $@"
	@$(COMPILE.c) $(CFLAGS_$*) -o $@ $<

%.rst: ../../../../Documentation/netlink/specs/%.yaml $(TOOL2)
	@echo -e "\tGEN_RST $@"
	@$(TOOL_RST) -o $@ -i $<

clean:
	rm -f *.o *.rst

distclean: clean
	rm -f *.c *.h *.a

regen:
	@../ynl-regen.sh

install-rsts:
	@echo -e "\tINSTALL generated docs"
	@$(INSTALL) -d $(DESTDIR)$(docdir)/ynl
	@$(INSTALL) -m 0644 $(RSTS) $(DESTDIR)$(docdir)/ynl/

install-specs:
	@echo -e "\tINSTALL specs"
	@$(INSTALL) -d $(DESTDIR)$(datarootdir)/ynl
	@$(INSTALL) -m 0644 ../../../../Documentation/netlink/*.yaml $(DESTDIR)$(datarootdir)/ynl/
	@$(INSTALL) -d $(DESTDIR)$(datarootdir)/ynl/specs
	@$(INSTALL) -m 0644 ../../../../Documentation/netlink/specs/*.yaml $(DESTDIR)$(datarootdir)/ynl/specs/

install: install-rsts install-specs

.PHONY: all clean distclean regen install
.DEFAULT_GOAL: all
