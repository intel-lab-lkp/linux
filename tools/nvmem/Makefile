# SPDX-License-Identifier: GPL-2.0-only
include ../scripts/Makefile.include

CC ?= $(CROSS_COMPILE)gcc
CFLAGS += -Wall -Wextra -O2

PROG := nvmemctl
SRCS := nvmemctl.c

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

install: $(PROG)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f $(PROG)

.PHONY: all install clean
