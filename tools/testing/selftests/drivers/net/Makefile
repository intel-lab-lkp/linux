# SPDX-License-Identifier: GPL-2.0

TEST_INCLUDES := $(wildcard lib/py/*.py) \
		 ../../net/net_helper.sh \
		 ../../net/lib.sh \

TEST_PROGS := \
	devmem.py \
	netcons_basic.sh \
	ping.py \
	queues.py \
	stats.py \
# end of TEST_PROGS

# YNL files, must be before "include ..lib.mk"
EXTRA_CLEAN += $(OUTPUT)/libynl.a $(OUTPUT)/ncdevmem
YNL_GEN_FILES := ncdevmem
TEST_GEN_FILES += $(YNL_GEN_FILES)

include ../../lib.mk

# YNL build
YNL_GENS := ethtool netdev
include ../../net/ynl.mk
