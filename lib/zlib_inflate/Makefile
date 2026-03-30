# SPDX-License-Identifier: GPL-2.0-only
#
# This is a modified version of zlib, which does all memory
# allocation ahead of time.
#
# This is only the decompression, see zlib_deflate for the
# the compression
#
# Decompression needs to be serialized for each memory
# allocation.
#
# (The upsides of the simplification is that you can't get in
# any nasty situations wrt memory management, and that the
# uncompression can be done without blocking on allocation).
#

# Force atomic GCOV counter updates to prevent GCC from merging global
# counters with loop induction variables — concurrent inflate_fast()
# execution on multiple CPUs causes out-of-bounds writes otherwise.
ifdef CONFIG_GCOV_KERNEL
ccflags-y += -fprofile-update=atomic
endif

obj-$(CONFIG_ZLIB_INFLATE) += zlib_inflate.o

zlib_inflate-objs := inffast.o inflate.o infutil.o \
		     inftrees.o inflate_syms.o
