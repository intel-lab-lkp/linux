# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Qualcomm DSP accelerator driver
#

obj-$(CONFIG_DRM_ACCEL_QDA)	:= qda.o

qda-y := \
	qda_cb.o \
	qda_drv.o \
	qda_ioctl.o \
	qda_memory_manager.o \
	qda_rpmsg.o

obj-$(CONFIG_DRM_ACCEL_QDA_COMPUTE_BUS) += qda_compute_bus.o
