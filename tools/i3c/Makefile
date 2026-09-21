# SPDX-License-Identifier: GPL-2.0-only
include ../scripts/Makefile.include

bindir ?= /usr/bin

ifeq ($(srctree),)
srctree := $(patsubst %/,%,$(dir $(CURDIR)))
srctree := $(patsubst %/,%,$(dir $(srctree)))
endif

# Do not use make's built-in rules
# (this improves performance and avoids hard-to-debug behaviour);
MAKEFLAGS += -r

CFLAGS += -O2 -Wall -g -D_GNU_SOURCE -I$(OUTPUT)include

ALL_TARGETS := i3ctransfer
ALL_PROGRAMS := $(patsubst %,$(OUTPUT)%,$(ALL_TARGETS))

all: $(ALL_PROGRAMS)

export srctree OUTPUT CC LD CFLAGS
include $(srctree)/tools/build/Makefile.include

#
# We need the following to be outside of kernel tree
#
$(OUTPUT)include/linux/i3c: ../../include/uapi/linux/i3c
	mkdir -p $(OUTPUT)include/linux/i3c 2>&1 || true
	ln -sf $(CURDIR)/../../include/uapi/linux/i3c/i3cdev.h $@

prepare: $(OUTPUT)include/linux/i3c

#
# i3ctransfer
#
I3CTRANSFER_IN := $(OUTPUT)i3ctransfer-in.o
$(I3CTRANSFER_IN): prepare FORCE
	$(Q)$(MAKE) $(build)=i3ctransfer
$(OUTPUT)i3ctransfer: $(I3CTRANSFER_IN)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm -f $(ALL_PROGRAMS)
	rm -rf $(OUTPUT)include/
	find $(or $(OUTPUT),.) -name '*.o' -delete
	find $(or $(OUTPUT),.) -name '\.*.o.d' -delete
	find $(or $(OUTPUT),.) -name '\.*.o.cmd' -delete

install: $(ALL_PROGRAMS)
	install -d -m 755 $(DESTDIR)$(bindir);		\
	for program in $(ALL_PROGRAMS); do		\
		install $$program $(DESTDIR)$(bindir);	\
	done

FORCE:

.PHONY: all install clean FORCE prepare
