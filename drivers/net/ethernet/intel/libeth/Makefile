# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2024 Intel Corporation

obj-$(CONFIG_LIBETH)		+= libeth.o

libeth-y			:= rx.o

obj-$(CONFIG_LIBETH_CP)		+= libeth_cp.o

libeth_cp-y			:= controlq.o

obj-$(CONFIG_LIBETH_PCI)	+= libeth_pci.o

libeth_pci-y			:= pci.o
