# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_NET_DSA_REALTEK)		+= realtek-dsa.o
realtek-dsa-objs-y			:= realtek-common.o
realtek-dsa-objs-$(CONFIG_NET_DSA_REALTEK_MDIO) += realtek-mdio.o
realtek-dsa-objs-$(CONFIG_NET_DSA_REALTEK_SMI) += realtek-smi.o
realtek-dsa-objs			:= $(realtek-dsa-objs-y)
obj-$(CONFIG_NET_DSA_REALTEK_RTL8366RB) += rtl8366.o
rtl8366-objs 				:= rtl8366-core.o rtl8366rb.o
obj-$(CONFIG_NET_DSA_REALTEK_RTL8365MB) += rtl8365mb.o
