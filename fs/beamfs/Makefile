# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the beamfs filesystem.
#

obj-$(CONFIG_BEAMFS_FS) += beamfs.o

beamfs-y := treecheck.o super.o inode.o dir.o dirent.o file.o \
	    file_inline.o scrub.o indparity.o rsbench.o budget.o clock.o \
	    alert.o edac.o alloc.o namei.o

# define_trace.h includes beamfs_trace.h by name, from super.c.
CFLAGS_super.o += -I$(src)

# The warnings the module is developed with. Whether they stop the
# build is left to CONFIG_WERROR.
ccflags-y += -Wall -Wextra -Wformat=2 -Werror=format
ccflags-y += -Wmissing-prototypes -Wmissing-declarations
ccflags-y += -Wundef -Wstrict-prototypes
# The VFS passes handlers more arguments than most of them use.
ccflags-y += -Wno-unused-parameter
