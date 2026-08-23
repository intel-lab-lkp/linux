# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the pNFS Flexfile Layout Driver kernel module
#
obj-$(CONFIG_PNFS_FLEXFILE_LAYOUT) += nfs_layout_flexfiles.o
nfs_layout_flexfiles-y := flexfilelayout.o flexfilelayoutdev.o

obj-$(CONFIG_NFS_FLEXFILE_DEVICEID_KUNIT_TEST) += nfs_flexfile_deviceid_kunit.o
nfs_flexfile_deviceid_kunit-y := tests/deviceid_kunit.o
