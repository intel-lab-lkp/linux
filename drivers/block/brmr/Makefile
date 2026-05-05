# SPDX-License-Identifier: GPL-2.0-or-later

ccflags-y := -I$(srctree)/drivers/infiniband/ulp/rtrs \
	     -I$(srctree)/drivers/infiniband/ulp/rmr \
	     -I$(srctree)/drivers/block/brmr

brmr-client-y := brmr-clt.o \
		 brmr-clt-sysfs.o \
		 brmr-clt-reque.o \
		 brmr-clt-stats.o

brmr-server-y := brmr-srv-sysfs.o \
		 brmr-srv.o

obj-$(CONFIG_BLK_DEV_BRMR_CLIENT) += brmr-client.o
obj-$(CONFIG_BLK_DEV_BRMR_SERVER) += brmr-server.o
