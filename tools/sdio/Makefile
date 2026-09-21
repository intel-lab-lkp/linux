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

CFLAGS += -O2 -Wall -g -D_GNU_SOURCE -I$(OUTPUT)include -I$(OUTPUT).

ALL_TARGETS := sdio-raw
ALL_PROGRAMS := $(patsubst %,$(OUTPUT)%,$(ALL_TARGETS))

all: $(ALL_PROGRAMS)

export srctree OUTPUT CC LD CFLAGS
include $(srctree)/tools/build/Makefile.include

#
# We need the SDIO UAPI header outside of the kernel tree
#
$(OUTPUT)include/linux/mmc/sdio_ioctl.h: FORCE
	mkdir -p $(OUTPUT)include/linux/mmc 2>&1 || true
	ln -sf $(CURDIR)/../../include/uapi/linux/mmc/sdio_ioctl.h $@

prepare: $(OUTPUT)include/linux/mmc/sdio_ioctl.h $(OUTPUT)sdio_ids_gen.h

#
# Name tables for the 'list' command, generated from the kernel's
# SDIO id database.
#
$(OUTPUT)sdio_ids_gen.h: gen-sdio-ids.sh $(srctree)/include/linux/mmc/sdio_ids.h
	$(Q)$(SHELL) $< $(srctree)/include/linux/mmc/sdio_ids.h > $@

#
# sdio-raw
#
SDIO_RAW_IN := $(OUTPUT)sdio-raw-in.o
$(SDIO_RAW_IN): prepare FORCE
	$(Q)$(MAKE) $(build)=sdio-raw
$(OUTPUT)sdio-raw: $(SDIO_RAW_IN) $(OUTPUT)sdio_ids_gen.h
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm -f $(ALL_PROGRAMS)
	rm -rf $(OUTPUT)include/
	rm -f $(OUTPUT)sdio_ids_gen.h
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
