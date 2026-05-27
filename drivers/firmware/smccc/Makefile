# SPDX-License-Identifier: GPL-2.0
#
obj-$(CONFIG_HAVE_ARM_SMCCC_DISCOVERY)	+= smccc.o kvm_guest.o
obj-$(CONFIG_ARM_SMCCC_SOC_ID)	+= soc_id.o

ifeq ($(CONFIG_HAVE_ARM_SMCCC_DISCOVERY),y)
obj-$(CONFIG_ARM64) += rmm.o
endif
