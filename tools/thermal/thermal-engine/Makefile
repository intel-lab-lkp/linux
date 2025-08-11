# SPDX-License-Identifier: GPL-2.0
# Makefile for thermal engine
include ../../build/Build.include

VERSION = 0.0.1

ifeq ($(srctree),)
srctree := $(patsubst %/,%,$(dir $(CURDIR)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
# $(info Determined 'srctree' to be $(srctree))
endif

BINDIR=usr/bin
WARNFLAGS=-Wall -Wshadow -W -Wformat -Wimplicit-function-declaration -Wimplicit-int
override CFLAGS+= $(call cc-option,-O3,-O1) ${WARNFLAGS}
# Add "-fstack-protector" only if toolchain supports it.
override CFLAGS+= $(call cc-option,-fstack-protector-strong)
CC?= $(CROSS_COMPILE)gcc
PKG_CONFIG?= $(CROSS_COMPILE)pkg-config

override CFLAGS+=-D VERSION=\"$(VERSION)\"
TARGET=thermal-engine

override CFLAGS += -I$(srctree)/tools/thermal/lib
override CFLAGS += -I$(srctree)/tools/lib/thermal/include

override LDFLAGS += -L$(srctree)/tools/thermal/lib
override LDFLAGS += -L$(srctree)/tools/lib/thermal

INSTALL_PROGRAM=install -m 755 -p
DEL_FILE=rm -f

# Static builds might require -ltinfo, for instance
ifneq ($(findstring -static, $(LDFLAGS)),)
STATIC := --static
endif

THERMAL_ENGINE_LIBS=-lconfig -lnl-genl-3 -lnl-3 -lthermal -lthermal_tools

OBJS = thermal-engine.o

thermal-engine: $(OBJS) Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS)  -o $(TARGET) $(THERMAL_ENGINE_LIBS)

install:
	- $(INSTALL_PROGRAM) -D "$(TARGET)" "$(INSTALL_ROOT)/$(BINDIR)/$(TARGET)"

uninstall:
	$(DEL_FILE) "$(INSTALL_ROOT)/$(BINDIR)/$(TARGET)"

clean:
	rm -f $(TARGET) $(OBJS)

dist:
	git tag v$(VERSION)
	git archive --format=tar --prefix="$(TARGET)-$(VERSION)/" v$(VERSION) | \
		gzip > $(TARGET)-$(VERSION).tar.gz
