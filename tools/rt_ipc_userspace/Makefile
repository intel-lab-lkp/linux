# SPDX-License-Identifier: GPL-2.0

CFLAGS = -Wall -Wextra -g -I.
PROGS := rt_ipc_server rt_ipc_client

all: $(PROGS)
%: %.c
	$(CC) $(CFLAGS) rt_ipc_action.c -o $@ $^

clean:
	$(RM) $(PROGS)

.PHONY: all clean
