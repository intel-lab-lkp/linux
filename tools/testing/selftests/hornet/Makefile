# SPDX-License-Identifier: GPL-2.0
include ../../../build/Build.include
include ../../../scripts/Makefile.arch
include ../../../scripts/Makefile.include

CLANG ?= clang
CFLAGS := -g -O2 -Wall
BPFTOOL ?= bpftool
SCRIPTSDIR := $(abspath ../../../../scripts/hornet)
TOOLSDIR := $(abspath ../../..)
LIBDIR := $(TOOLSDIR)/lib
BPFDIR := $(LIBDIR)/bpf
TOOLSINCDIR := $(TOOLSDIR)/include
APIDIR := $(TOOLSINCDIR)/uapi
CERTDIR := $(abspath ../../../../certs)

TEST_GEN_PROGS_EXTENDED := loader
TEST_GEN_PROGS := signed_loader
TEST_PROGS := fail_loader
TEST_GEN_FILES := vmlinux.h loader.h trivial.bin trivial.bpf.o
$(TEST_GEN_PROGS): LDLIBS += -lbpf
$(TEST_GEN_PROGS): $(TEST_GEN_FILES)

include ../lib.mk

BPF_CFLAGS := -target bpf \
              -D__TARGET_ARCH_$(ARCH) \
              -I/usr/include/$(shell uname -m)-linux-gnu \
               $(KHDR_INCLUDES)
vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

trivial.bpf.o: trivial.bpf.c vmlinux.h
	$(CLANG) $(CFLAGS) $(BPF_CFLAGS) -c $< -o $@

loader.h: trivial.bpf.o
	$(BPFTOOL) gen skeleton -L $< name trivial > $@

trivial.bin: loader.h
	$(SCRIPTSDIR)/extract-skel.sh $< $@

loader: loader.c loader.h
	$(CC) $(CFLAGS) -I$(LIBDIR) -I$(APIDIR) $< -o $@ -lbpf

fail_loader: fail_loader.c loader.h
	$(CC) $(CFLAGS) -I$(LIBDIR) -I$(APIDIR) $< -o $@ -lbpf

signed_loader: trivial.bin loader fail_loader
	$(SCRIPTSDIR)/sign-ebpf sha256 $(CERTDIR)/signing_key.pem  $(CERTDIR)/signing_key.x509 \
		trivial.bin loader signed_loader
	chmod u+x $@
