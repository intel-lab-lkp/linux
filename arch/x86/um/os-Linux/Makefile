#
# Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
# Licensed under the GPL
#

obj-y = mcontext.o

obj-$(CONFIG_X86_32) += tls.o

USER_OBJS := $(obj-y)

obj-y += registers.o
NOLIBC_OBJS := registers.o

include $(srctree)/arch/um/scripts/Makefile.rules
