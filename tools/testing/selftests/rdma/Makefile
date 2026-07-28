# SPDX-License-Identifier: GPL-2.0
TEST_GEN_FILES := rxe_mr_overflow
TEST_PROGS := rxe_mr_overflow.sh \
		rxe_rping_between_netns.sh \
		rxe_ipv6.sh \
		rxe_socket_with_netns.sh \
		rxe_test_NETDEV_UNREGISTER.sh \
		rxe_sent_rcvd_bytes.sh

include ../lib.mk

# rxe_mr_overflow uses libibverbs / librdmacm
$(OUTPUT)/rxe_mr_overflow: CFLAGS += -O2 -Wall
$(OUTPUT)/rxe_mr_overflow: LDLIBS += -libverbs -lrdmacm
