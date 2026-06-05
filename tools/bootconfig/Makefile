# SPDX-License-Identifier: GPL-2.0
# Makefile for bootconfig command
include ../scripts/Makefile.include

bindir ?= /usr/bin

ifeq ($(srctree),)
srctree := $(patsubst %/,%,$(dir $(CURDIR)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
endif

LIBSRC = $(srctree)/lib/bootconfig.c $(srctree)/include/linux/bootconfig.h
override CFLAGS += -Wall -g -I$(CURDIR)/include

ALL_TARGETS := bootconfig
ALL_PROGRAMS := $(patsubst %,$(OUTPUT)%,$(ALL_TARGETS))

all: $(ALL_PROGRAMS)

# bootconfig is a build host tool: Kbuild's prepare hook runs it on the
# build machine to render the embedded cmdline, so always compile it with
# $(HOSTCC). Using $(CC) would cross-compile it under ARCH=... builds and
# fail to exec on the host ("Exec format error").
$(OUTPUT)bootconfig: main.c include/linux/bootconfig.h $(LIBSRC)
	$(HOSTCC) $(filter %.c,$^) $(CFLAGS) $(LDFLAGS) -o $@

test: $(ALL_PROGRAMS) test-bootconfig.sh
	./test-bootconfig.sh $(OUTPUT)

install: $(ALL_PROGRAMS)
	install $(OUTPUT)bootconfig $(DESTDIR)$(bindir)

clean:
	rm -f $(OUTPUT)*.o $(ALL_PROGRAMS)
