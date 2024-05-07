# SPDX-License-Identifier: GPL-2.0
# Makefile for Intel Software Defined Silicon provisioning tool
include ../../../scripts/Makefile.include
BINDIR ?= /usr/sbin

SRCS = intel_sdsi.c spdm.c

OBJS = $(SRCS:.c=.o)

override CFLAGS += -O2 -Wall -Wextra -I../../../include

intel_sdsi: intel_sdsi.h $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

intel_sdsi.h: ../../../../include/uapi/linux/intel_sdsi.h
	ln -sf ../../../../include/uapi/linux/intel_sdsi.h $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean :
	@rm -f intel_sdsi intel_sdsi.h $(OBJS)

install : intel_sdsi
	install -d  $(DESTDIR)$(BINDIR)
	install -m 755 -p intel_sdsi $(DESTDIR)$(BINDIR)/intel_sdsi

.PHONY : clean install
