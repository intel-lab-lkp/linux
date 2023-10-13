# SPDX-License-Identifier: GPL-2.0
#
# Makefile for CDX
#
# Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
#

obj-$(CONFIG_CDX_BUS) += cdx.o controller/

ifdef CONFIG_CDX_BUS
obj-$(CONFIG_GENERIC_MSI_IRQ) += cdx_msi.o
endif
