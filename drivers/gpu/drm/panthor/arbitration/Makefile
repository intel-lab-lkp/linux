# SPDX-License-Identifier: GPL-2.0 OR MIT
# Copyright 2026 ARM Limited. All rights reserved.

obj-$(CONFIG_DRM_PANTHOR_ARBITRATION) += panthor_arbitration.o
panthor_arbitration-y := \
	panthor_arbitration_drv.o \
	panthor_arbitration_sched.o \
	panthor_partition_control.o \
	panthor_resource_group.o

INCLUDES = \
	-I$(src)/..

ccflags-y += $(INCLUDES)
subdir-ccflags-y += $(INCLUDES)


