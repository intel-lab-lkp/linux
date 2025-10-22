# SPDX-License-Identifier: GPL-2.0

# tell define_trace.h where to find the trace header
CFLAGS_wave6-vpu-core.o := -I$(src)

wave6-objs += wave6-vpu.o \
	      wave6-vpu-thermal.o
obj-$(CONFIG_VIDEO_WAVE6_VPU) += wave6.o

wave6-core-objs += wave6-vpu-core.o \
		   wave6-vpu-v4l2.o \
		   wave6-vpu-dbg.o \
		   wave6-vpuapi.o \
		   wave6-vpu-dec.o \
		   wave6-vpu-enc.o \
		   wave6-hw.o
obj-$(CONFIG_VIDEO_WAVE6_VPU) += wave6-core.o
