# SPDX-License-Identifier: GPL-2.0-or-later

ccflags-y := -I$(srctree)/drivers/infiniband/ulp/rtrs

CFLAGS_rmr-clt-trace.o = -I$(src)

rmr-client-y := rmr-pool.o \
		rmr-clt.o \
		rmr-map-mgmt.o \
		rmr-clt-stats.o \
		rmr-clt-sysfs.o \
		rmr-map.o \
		rmr-clt-trace.o

rmr-server-y := rmr-pool.o \
		rmr-srv.o \
		rmr-srv-md.o \
		rmr-srv-sysfs.o \
		rmr-req.o \
		rmr-map.o

obj-$(CONFIG_INFINIBAND_RMR_CLIENT) += rmr-client.o
obj-$(CONFIG_INFINIBAND_RMR_SERVER) += rmr-server.o
