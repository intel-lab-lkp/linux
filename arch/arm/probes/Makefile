# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_UPROBES)		+= decode.o decode-arm.o uprobes/
obj-$(CONFIG_KPROBES)		+= decode.o kprobes/
obj-$(CONFIG_KPROBES_ON_FTRACE)	+= ftrace.o
ifdef CONFIG_THUMB2_KERNEL
obj-$(CONFIG_KPROBES)		+= decode-thumb.o
else
obj-$(CONFIG_KPROBES)		+= decode-arm.o
endif
