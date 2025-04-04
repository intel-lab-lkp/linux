# SPDX-License-Identifier: GPL-2.0
hostprogs-always-y	:= sign-ebpf

HOSTCFLAGS_sign-ebpf.o = $(shell $(HOSTPKG_CONFIG) --cflags libcrypto 2> /dev/null)
HOSTLDLIBS_sign-ebpf = $(shell $(HOSTPKG_CONFIG) --libs libcrypto 2> /dev/null || echo -lcrypto)
