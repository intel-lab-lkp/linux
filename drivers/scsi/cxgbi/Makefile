# SPDX-License-Identifier: GPL-2.0-only

CONTEXT_ANALYSIS := 1

ccflags-y += -I $(srctree)/drivers/net/ethernet/chelsio/libcxgb

obj-$(CONFIG_SCSI_CXGB3_ISCSI)	+= libcxgbi.o cxgb3i/
obj-$(CONFIG_SCSI_CXGB4_ISCSI)	+= libcxgbi.o cxgb4i/
