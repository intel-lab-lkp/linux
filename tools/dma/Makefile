# SPDX-License-Identifier: GPL-2.0
bindir ?= /usr/bin

CFLAGS += -idirafter../../include/uapi

TARGET = dma_map_benchmark

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $< -o $@

install: all
	install -D -m 755 $(TARGET) $(DESTDIR)$(bindir)/$(TARGET)

clean:
	rm -f $(TARGET)
