# SPDX-License-Identifier: GPL-2.0 OR MIT
# Copyright 2026 ARM Limited. All rights reserved.

obj-$(CONFIG_DRM_PANTHOR_SYSTEM) += panthor_system.o

panthor_system-y := \
	panthor_system_drv.o

INCLUDES = \
	-I$(src)/..

ccflags-y += $(INCLUDES)
subdir-ccflags-y += $(INCLUDES)
