# SPDX-License-Identifier: GPL-2.0
include ../../../../../scripts/Makefile.include

CC	= clang
INSTALL ?= install

bpf-objs-y := $(patsubst %.c,%.o,$(wildcard */*.c))

CFLAGS += -I../../../../../bpf/bpftool/ -I../../../../../bpf/bpftool/libbpf/include
CFLAGS += -Iinclude -Ispi

all: $(bpf-objs-y)

%.o: %.c
	$(CC) -target bpf -Wall -O2 $(CFLAGS) -g -c $< -o $@

install:
	$(INSTALL) -m 0755 -d $(INSTALL_PATH)
	$(INSTALL) $(bpf-objs-y) $(INSTALL_PATH)

clean:
	rm -rf $(bpf-objs-y)
