# SPDX-License-Identifier: GPL-2.0

all:
	@echo mk_build_dir="$(shell pwd)" > include.sh

TEST_PROGS := \
	rds_tcp_run.sh \
	rds_rdma_run.sh \
# end of TEST_PROGS

TEST_FILES := \
	rds_run.sh \
	include.sh \
	settings \
	test.py \
# end of TEST_FILES

EXTRA_CLEAN := \
	include.sh \
	/tmp/rds_logs \
# end of EXTRA_CLEAN

include ../../lib.mk
