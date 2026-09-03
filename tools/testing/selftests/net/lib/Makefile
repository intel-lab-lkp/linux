# SPDX-License-Identifier: GPL-2.0

CFLAGS += -Wall -Wl,--no-as-needed -O2 -g
CFLAGS += -I../../../../../usr/include/ $(KHDR_INCLUDES)
# Additional include paths needed by kselftest.h
CFLAGS += -I../../

TEST_FILES := \
	../../../../net/ynl \
	../../../../../Documentation/netlink/specs \
	ksft_setup_loopback.sh \
# end of TEST_FILES

TEST_GEN_FILES := \
	$(patsubst %.c,%.o,$(wildcard *.bpf.c)) \
	csum \
	gro \
	xdp_helper \
# end of TEST_GEN_FILES

TEST_INCLUDES := $(wildcard py/*.py sh/*.sh)

include ../../lib.mk

# gro optionally needs OpenSSL for PSP.
PKG_CONFIG ?= pkg-config
LIBCRYPTO_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcrypto 2>/dev/null)
LIBCRYPTO_LIBS := $(shell $(PKG_CONFIG) --libs libcrypto 2>/dev/null || \
		    echo -lcrypto)
HAVE_OPENSSL := $(shell echo 'int main(void) { return 0; }' | \
		  $(CC) -x c - -include openssl/evp.h $(LIBCRYPTO_CFLAGS) \
		  $(LIBCRYPTO_LIBS) -o /dev/null >/dev/null 2>&1 && echo 1)

ifeq ($(HAVE_OPENSSL),1)
$(OUTPUT)/gro: CFLAGS += -DHAVE_OPENSSL $(LIBCRYPTO_CFLAGS)
$(OUTPUT)/gro: LDLIBS += $(LIBCRYPTO_LIBS)
else
$(warning libcrypto not found, gro will be built without PSP support)
endif

include ../bpf.mk
