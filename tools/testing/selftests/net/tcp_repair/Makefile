# SPDX-License-Identifier: GPL-2.0
#
# selftests/net/tcp_repair: TCP_REPAIR connection tests
#
# Makefile - Build test client and server, declare run.sh entrypoint
#
# Copyright (c) 2026 Red Hat GmbH
#
# Author: Stefano Brivio <sbrivio@redhat.com>

top_srcdir = ../../../../..

CFLAGS += -Wall -Wextra -pedantic

TEST_PROGS := \
	run.sh

TEST_GEN_FILES := \
	client \
	server \
# end of TEST_GEN_FILES

include ../../lib.mk
