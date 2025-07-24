# SPDX-License-Identifier: GPL-2.0
bindir ?= /usr/bin

CFLAGS += -I../../include -I../../usr/include

all:
	$(CC) $(CFLAGS) dma_map_benchmark.c -o dma_map_benchmark

install: all
	install -D dma_map_benchmark $(bindir)/bin/dma_map_benchmark

clean:
	rm -f dma_map_benchmark

.PHONY: all install clean
