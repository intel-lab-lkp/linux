# SPDX-License-Identifier: GPL-2.0-only

include ../Makefile.deps

INSTALL	?= install
prefix  ?= /usr

CC := gcc
CFLAGS := -Wall -Wextra -Werror -O2
ifeq ("$(DEBUG)","1")
  CFLAGS += -g -fsanitize=address -fsanitize=leak -static-libasan
endif
CFLAGS += -I../lib

SRCS := $(wildcard *.c)
OBJS := $(patsubst %.c,$(OUTPUT)%.o,$(SRCS))

YNLTOOL := $(OUTPUT)ynltool

include $(wildcard *.d)

all: $(YNLTOOL)

$(YNLTOOL): $(OBJS)
	@echo -e "\tLINK $@"
	@$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c main.h json_writer.h
	@echo -e "\tCC $@"
	@$(COMPILE.c) -MMD -c -o $@ $<

clean:
	rm -f *.o *.d *~

distclean: clean
	rm -f $(YNLTOOL)

bindir ?= /usr/bin

install: $(YNLTOOL)
	install -m 0755 $(YNLTOOL) $(DESTDIR)$(bindir)/$(YNLTOOL)

.PHONY: all clean distclean
.DEFAULT_GOAL=all
