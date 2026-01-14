# SPDX-License-Identifier: GPL-2.0-or-later

CC?=$(CROSS_COMPILE)gcc
OBJS = stlm.o

CFLAGS=-Wall -static -std=gnu11 -I ../../../include/uapi/
ifneq ($(DEBUG), )
	CFLAGS+=-O0 -g -ggdb
else
	CFLAGS+=-static
endif

all: stlm

stlm: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o
	rm -f stlm

.PHONY: clean
