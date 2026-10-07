# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_HYPERV_NET) += hv_netvsc.o

hv_netvsc-y := netvsc_drv.o netvsc.o rndis_filter.o netvsc_trace.o netvsc_bpf.o
hv_netvsc-$(CONFIG_HYPERV_NET_KUNIT_TEST) += rndis_request_test.o
