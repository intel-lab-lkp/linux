# SPDX-License-Identifier: GPL-2.0-only
#
# This is a modified version of zlib, which does all memory
# allocation ahead of time.
#
# This is the compression code, see zlib_inflate for the
# decompression code.
#

# Force atomic GCOV counter updates to prevent GCC from merging global
# counters with loop induction variables (see lib/zlib_inflate/Makefile).
ifdef CONFIG_GCOV_KERNEL
ccflags-y += -fprofile-update=atomic
endif

obj-$(CONFIG_ZLIB_DEFLATE) += zlib_deflate.o

zlib_deflate-objs := deflate.o deftree.o deflate_syms.o
