#! /bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Runs the RDS selftest over the RDMA (RoCE/RXE) transport.
#
# This is a wrapper script for rds_run.sh to run and report results when using
# the -T rdma option
#
# Exits with the kselftest SKIP code if rds RDMA prerequisites are not met

exec "$(dirname "$0")/rds_run.sh" -T rdma "$@"
