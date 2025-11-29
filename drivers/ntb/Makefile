# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_NTB) += ntb.o hw/ test/
obj-$(CONFIG_NTB_TRANSPORT) += ntb_transport.o

ntb-y			:= core.o
ntb-$(CONFIG_NTB_MSI)	+= msi.o

ntb_transport-y					:= ntb_transport_core.o
ntb_transport-$(CONFIG_NTB_TRANSPORT_EDMA)	+= ntb_edma.o
