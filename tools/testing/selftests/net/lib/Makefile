# SPDX-License-Identifier: GPL-2.0

CFLAGS += -Wall -Wl,--no-as-needed -O2 -g
CFLAGS += -I../../../../../usr/include/ $(KHDR_INCLUDES)
# Additional include paths needed by kselftest.h
CFLAGS += -I../../

# xskxceiver needs a working bpftool to generate its skeleton
BPFTOOL ?= bpftool
HAS_BPFTOOL := $(shell $(BPFTOOL) version >/dev/null 2>&1 && echo y)
ifeq ($(HAS_BPFTOOL),y)
COND_GEN_FILES += xskxceiver
else
$(warning excluding xskxceiver, bpftool not installed or not working)
endif

TEST_FILES := \
	../../../../net/ynl \
	../../../../../Documentation/netlink/specs \
	ksft_setup_loopback.sh \
# end of TEST_FILES

TEST_GEN_FILES := \
	$(patsubst %.c,%.o,$(wildcard *.bpf.c)) \
	$(COND_GEN_FILES) \
	csum \
	gro \
	xdp_helper \
# end of TEST_GEN_FILES

TEST_INCLUDES := $(wildcard py/*.py sh/*.sh) xsk/test_xsk_case_defs.h

include ../../lib.mk

include ../bpf.mk

# AF_XDP test engine used by net/test_xsk.sh.
XSK_DIR := xsk
XSK_BPF_OBJ := $(OUTPUT)/xsk_xdp_progs.bpf.o
XSK_SKEL := $(OUTPUT)/xsk_xdp_progs.skel.h
XSK_SOURCES := $(addprefix $(XSK_DIR)/,xskxceiver.c xsk.c test_xsk.c xsk_peer.c) \
	$(top_srcdir)/tools/lib/find_bit.c
XSK_HEADERS := $(wildcard $(XSK_DIR)/*.h)

PKG_CONFIG ?= $(CROSS_COMPILE)pkg-config
XSK_LDLIBS := $(shell $(PKG_CONFIG) libelf --libs 2>/dev/null || echo -lelf) -lz

$(XSK_BPF_OBJ): $(XSK_DIR)/xsk_xdp_progs.bpf.c $(XSK_DIR)/xsk_xdp_common.h $(BPFOBJ)
	$(call msg,BPF_PROG,,$@)
	$(Q)$(CLANG) -O2 -g --target=bpf $(CCINCLUDE) $(CLANG_SYS_INCLUDES) \
		-Wno-compare-distinct-pointer-types -c $< -o $@

$(OUTPUT)/xskxceiver: $(XSK_SOURCES) $(XSK_HEADERS) $(XSK_SKEL) $(BPFOBJ)
	$(call msg,CC,,$@)
	$(Q)$(CC) $(CFLAGS) -I$(top_srcdir)/tools/include -I$(OUTPUT) \
		-I$(SCRATCH_DIR)/include \
		$(XSK_SOURCES) $(BPFOBJ) $(LDLIBS) $(XSK_LDLIBS) -o $@

EXTRA_CLEAN += $(XSK_BPF_OBJ) $(XSK_SKEL)
