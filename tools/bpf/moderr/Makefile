# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
include ../../scripts/Makefile.include
include ../../scripts/Makefile.arch

OUTPUT ?= $(abspath .output)/

BPFTOOL_OUTPUT := $(OUTPUT)bpftool/
DEFAULT_BPFTOOL := $(BPFTOOL_OUTPUT)bootstrap/bpftool
BPFTOOL ?= $(DEFAULT_BPFTOOL)
LIBBPF_SRC := $(abspath ../../lib/bpf)
BPFOBJ_OUTPUT := $(OUTPUT)libbpf/
BPFOBJ := $(BPFOBJ_OUTPUT)libbpf.a
BPF_DESTDIR := $(BPFOBJ_OUTPUT)
BPF_INCLUDE := $(BPF_DESTDIR)include
INCLUDES := -I$(OUTPUT) -I$(BPF_INCLUDE) -I$(abspath ../../include/uapi)
CFLAGS := -g -Wall $(CLANG_CROSS_FLAGS)
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS += -lelf -lz

# Try to detect best kernel BTF source
KERNEL_REL := $(shell uname -r)
VMLINUX_BTF_PATHS := $(if $(O),$(O)/vmlinux)		\
	$(if $(KBUILD_OUTPUT),$(KBUILD_OUTPUT)/vmlinux) \
	../../../vmlinux /sys/kernel/btf/vmlinux	\
	/boot/vmlinux-$(KERNEL_REL)
VMLINUX_BTF_PATH := $(or $(VMLINUX_BTF),$(firstword			       \
					  $(wildcard $(VMLINUX_BTF_PATHS))))

ifeq ($(V),1)
Q =
else
Q = @
MAKEFLAGS += --no-print-directory
submake_extras := feature_display=0
endif

.DELETE_ON_ERROR:

.PHONY: all clean moderr libbpf_hdrs
all: moderr

moderr: $(OUTPUT)moderr

clean:
	$(call QUIET_CLEAN, moderr)
	$(Q)$(RM) -r $(BPFOBJ_OUTPUT) $(BPFTOOL_OUTPUT)
	$(Q)$(RM) $(OUTPUT)*.o $(OUTPUT)*.d
	$(Q)$(RM) $(OUTPUT)*.skel.h $(OUTPUT)vmlinux.h
	$(Q)$(RM) $(OUTPUT)moderr
	$(Q)$(RM) -r .output

libbpf_hdrs: $(BPFOBJ)

$(OUTPUT)moderr: $(OUTPUT)moderr.o $(BPFOBJ)
	$(QUIET_LINK)$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OUTPUT)moderr.o: moderr.h $(OUTPUT)moderr.skel.h	      \
			$(OUTPUT)moderr.bpf.o | libbpf_hdrs

$(OUTPUT)moderr.bpf.o: $(OUTPUT)vmlinux.h moderr.h | libbpf_hdrs

$(OUTPUT)%.skel.h: $(OUTPUT)%.bpf.o | $(BPFTOOL)
	$(QUIET_GEN)$(BPFTOOL) gen skeleton $< > $@

$(OUTPUT)%.bpf.o: %.bpf.c $(BPFOBJ) | $(OUTPUT)
	$(QUIET_GEN)$(CLANG) -g -O2 --target=bpf $(INCLUDES) \
		 -D__TARGET_ARCH_$(SRCARCH)						 \
		 -c $(filter %.c,$^) -o $@ &&				     \
	$(LLVM_STRIP) -g $@

$(OUTPUT)%.o: %.c | $(OUTPUT)
	$(QUIET_CC)$(CC) $(CFLAGS) $(INCLUDES) -c $(filter %.c,$^) -o $@

$(OUTPUT) $(BPFOBJ_OUTPUT) $(BPFTOOL_OUTPUT):
	$(QUIET_MKDIR)mkdir -p $@

$(OUTPUT)vmlinux.h: $(VMLINUX_BTF_PATH) | $(OUTPUT) $(BPFTOOL)
ifeq ($(VMLINUX_H),)
	$(Q)if [ ! -e "$(VMLINUX_BTF_PATH)" ] ; then \
		echo "Couldn't find kernel BTF; set VMLINUX_BTF to"	       \
			"specify its location." >&2;			       \
		exit 1;\
	fi
	$(QUIET_GEN)$(BPFTOOL) btf dump file $(VMLINUX_BTF_PATH) format c > $@
else
	$(Q)cp "$(VMLINUX_H)" $@
endif

$(BPFOBJ): $(wildcard $(LIBBPF_SRC)/*.[ch] $(LIBBPF_SRC)/Makefile) | $(BPFOBJ_OUTPUT)
	$(Q)$(MAKE) $(submake_extras) -C $(LIBBPF_SRC) OUTPUT=$(BPFOBJ_OUTPUT) \
		    DESTDIR=$(BPFOBJ_OUTPUT) prefix= $(abspath $@) install_headers

$(DEFAULT_BPFTOOL): | $(BPFTOOL_OUTPUT)
	$(Q)$(MAKE) $(submake_extras) -C ../bpftool OUTPUT=$(BPFTOOL_OUTPUT) bootstrap
