# SPDX-License-Identifier: GPL-2.0
# Makefile for Intel Software Defined Silicon provisioning tool
include ../../../scripts/Makefile.include
BINDIR ?= /usr/sbin

SRCS = intel_sdsi.c

OBJS = $(SRCS:.c=.o)

override CFLAGS += -O2 -Wall -Wextra

intel_sdsi: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean :
	@rm -f intel_sdsi $(OBJS)

install : intel_sdsi
	install -d  $(DESTDIR)$(BINDIR)
	install -m 755 -p intel_sdsi $(DESTDIR)$(BINDIR)/intel_sdsi

.PHONY : clean install
