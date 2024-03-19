# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for PPS generators.
#

obj-$(CONFIG_PPS_GENERATOR_PARPORT) += pps_gen_parport.o
obj-$(CONFIG_PPS_GENERATOR_TIO) += pps_gen_tio.o

ifeq ($(CONFIG_PPS_DEBUG),y)
EXTRA_CFLAGS += -DDEBUG
endif
