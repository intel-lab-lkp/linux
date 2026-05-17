#!/bin/sh -euf
# SPDX-License-Identifier: GPL-2.0
#
# selftests/net/tcp_repair: TCP_REPAIR connection tests
#
# run.sh - Test entry point: detach outer namespace and run outer.sh in it
#
# Copyright (c) 2026 Red Hat GmbH
#
# Author: Stefano Brivio <sbrivio@redhat.com>

unshare -rUn -- ./outer.sh
