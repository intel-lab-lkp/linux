# SPDX-License-Identifier: GPL-2.0-only
INCLUDE = -I$(KERNEL_SOURCE)/include	\
	-I$(KERNEL_SOURCE)/drivers/scsi	\
	-I$(PWD)

ccflags-y += -g $(INCLUDE)

OBJ = linux_os/linux_base.o linux_os/linux_api.o linux_os/linux_scsi.o
CARD_OBJ = card/cardcommon.o card/cardinterface.o card/mmc.o card/sd.o card/thermal.o card/uhs2.o card/card_ddr200_support.o card/output_tuning.o
HOST_OBJ = host/host.o host/hostven.o host/cmdhandler.o host/irqhandler.o host/transhandler.o
MAIN_OBJ = main/cfgmng.o main/thread.o main/autotimerfunc.o main/geniofunc.o main/pmfunc.o main/reqmng.o main/testcase.o
TQ_OBJ = tagqueue/tagqueue.o  tagqueue/tqadma2.o tagqueue/tqadma3.o tagqueue/tqadma_sdma_like.o tagqueue/tqpolicy.o tagqueue/tqsdma.o tagqueue/tq_merge.o
UTIL_OBJ = util/util.o util/debug.o

obj-$(CONFIG_SCSI_BHT) := bht-sd.o
bht-sd-objs := $(OBJ) $(CARD_OBJ) $(HOST_OBJ) $(MAIN_OBJ) $(TQ_OBJ) $(UTIL_OBJ)


