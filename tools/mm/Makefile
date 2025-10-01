# SPDX-License-Identifier: GPL-2.0
# Makefile for vm tools
#
include ../scripts/Makefile.include

BUILD_TARGETS=page-types slabinfo page_owner_sort thp_swap_allocator_test \
	      ksm-utils
INSTALL_TARGETS = $(BUILD_TARGETS) thpmaps

LIB_DIR = ../lib/api
LIBS = $(LIB_DIR)/libapi.a

CFLAGS += -Wall -Wextra -I../lib/ -pthread
LDFLAGS += $(LIBS) -pthread

all: $(BUILD_TARGETS) ksm-utils

$(BUILD_TARGETS): $(LIBS)

$(LIBS):
	make -C $(LIB_DIR)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:  ksm-utils_clean
	$(RM) page-types slabinfo page_owner_sort thp_swap_allocator_test
	make -C $(LIB_DIR) clean

ksm-utils:
	$(call descend,ksm-utils)
ksm-utils_clean:
	$(call descend,ksm-utils,clean)

sbindir ?= /usr/sbin

install: all
	install -d $(DESTDIR)$(sbindir)
	install -m 755 -p $(INSTALL_TARGETS) $(DESTDIR)$(sbindir)
