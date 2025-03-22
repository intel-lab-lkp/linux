# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Linux minix filesystem routines.
#

obj-$(CONFIG_MINIX_FS) += minix.o

minix-objs := bitmap.o itree_v1.o itree_v2.o namei.o inode.o file.o dir.o

# If we are built-in, we provide support for minix filesystem on initrds.
ifeq ($(CONFIG_MINIX_FS),y)
minix-objs += initrd.o
endif
