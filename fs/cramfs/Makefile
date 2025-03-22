# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the linux cramfs routines.
#

obj-$(CONFIG_CRAMFS) += cramfs.o

cramfs-objs := inode.o uncompress.o

# If we are built-in, we provide support for cramfs on initrds.
ifeq ($(CONFIG_CRAMFS),y)
cramfs-objs += initrd.o
endif
