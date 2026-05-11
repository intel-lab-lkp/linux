# SPDX-License-Identifier: GPL-2.0
# Builds tlob selftest helper binaries.
#
# Invoked by ../Makefile; pass OUTDIR to control the output directory
# and TOOLS_INCLUDES for the in-tree UAPI -isystem flag.

OUTDIR ?= $(CURDIR)/..
CFLAGS += $(TOOLS_INCLUDES)

.PHONY: all
all: $(OUTDIR)/tlob_ioctl $(OUTDIR)/tlob_target

$(OUTDIR)/tlob_ioctl: tlob_ioctl.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

$(OUTDIR)/tlob_target: tlob_target.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	$(RM) $(OUTDIR)/tlob_ioctl $(OUTDIR)/tlob_target
