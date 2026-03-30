# SPDX-License-Identifier: GPL-2.0-only
#
# This is a modified version of zlib, which does all memory
# allocation ahead of time.
#
# This is the code for s390 zlib hardware support.
#

# Force atomic GCOV counter updates to prevent GCC from merging global
# counters with loop induction variables (see lib/zlib_inflate/Makefile).
ifdef CONFIG_GCOV_KERNEL
ccflags-y += -fprofile-update=atomic
endif

obj-$(CONFIG_ZLIB_DFLTCC) += zlib_dfltcc.o

zlib_dfltcc-objs := dfltcc.o dfltcc_deflate.o dfltcc_inflate.o
