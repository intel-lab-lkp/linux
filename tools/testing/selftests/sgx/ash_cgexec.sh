#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2024 Intel Corporation.

# Start a program in a given cgroup.
# Supports V2 cgroup paths, relative to /sys/fs/cgroup
if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <v2 cgroup path> <command> [args...]"
    exit 1
fi
# Move this shell to the cgroup.
echo 0 >/sys/fs/cgroup/$1/cgroup.procs
shift
# Execute the command within the cgroup
exec "$@"

