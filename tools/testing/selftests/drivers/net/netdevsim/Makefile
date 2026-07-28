# SPDX-License-Identifier: GPL-2.0+ OR MIT

CFLAGS += $(KHDR_INCLUDES)

TEST_PROGS := \
	devlink.sh \
	devlink_in_netns.sh \
	devlink_trap.sh \
	ethtool-coalesce.sh \
	ethtool-features.sh \
	ethtool-fec.sh \
	ethtool-pause.sh \
	fib.sh \
	fib_notifications.sh \
	hw_stats_l3.sh \
	nexthop.sh \
	peer.sh \
	psample.sh \
	tc-mq-visibility.sh \
	tls.sh \
	udp_tunnel_nic.sh \
# end of TEST_PROGS

TEST_GEN_FILES := \
	tls_offload
# end of TEST_GEN_FILES

TEST_FILES := \
	ethtool-common.sh
# end of TEST_FILES

include ../../../lib.mk
