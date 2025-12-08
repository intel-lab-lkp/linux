# SPDX-License-Identifier: GPL-2.0

al300-vdec-objs := al_codec_common.o al_codec_util.o al_vdec_drv.o

ifneq ($(CONFIG_DEBUG_FS),)
al300-vdec-objs += al_codec_dbgfs.o
endif

obj-$(CONFIG_VIDEO_ALLEGRO_DVT_D300) += al300-vdec.o
