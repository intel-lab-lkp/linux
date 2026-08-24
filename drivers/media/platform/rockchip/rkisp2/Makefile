# SPDX-License-Identifier: GPL-2.0-only

rockchip-isp2-y := rkisp2-capture.o \
           rkisp2-common.o \
           rkisp2-dev.o \
           rkisp2-dmarx.o \
           rkisp2-isp.o \
           rkisp2-params.o \
	   rkisp2-resizer.o \
           rkisp2-stats.o

rockchip-isp2-$(CONFIG_DEBUG_FS) += rkisp2-debug.o

obj-$(CONFIG_VIDEO_ROCKCHIP_ISP2) += rockchip-isp2.o
