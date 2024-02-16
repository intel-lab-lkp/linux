# SPDX-License-Identifier: GPL-2.0

top_srcdir = ../../../../..

CFLAGS =  -Wall -Wl,--no-as-needed -O2 -g -I$(top_srcdir)/usr/include $(KHDR_INCLUDES)

TEST_PROGS := openvswitch.sh

TEST_FILES := ovs-dpctl.py

OVS_TESTS := \
	test_arp_ping \
	test_ct_connect_v4 \
	test_connect_v4 \
	test_nat_connect_v4 \
	test_nat_related_v4 \
	test_netlink_checks \
	test_upcall_interfaces \
	test_drop_reason

EXTRA_CLEAN := $(OVS_TESTS)

include ../../lib.mk
