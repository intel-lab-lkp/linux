# SPDX-License-Identifier: GPL-2.0
#! /bin/bash

set -e
set -u
set -x

unset KBUILD_OUTPUT

# start with a default config
make defconfig

# no modules
scripts/config --disable CONFIG_MODULES

# enable RDS
scripts/config --enable CONFIG_RDS
scripts/config --enable CONFIG_RDS_TCP

# instrument RDS and only RDS
scripts/config --enable CONFIG_GCOV_KERNEL
scripts/config --disable GCOV_PROFILE_ALL
scripts/config --enable GCOV_PROFILE_RDS

# need network namespaces to run tests with veth network interfaces
scripts/config --enable CONFIG_NET_NS
scripts/config --enable CONFIG_VETH

# simulate packet loss
scripts/config --enable CONFIG_NET_SCH_NETEM

# generate real .config without asking any questions
make olddefconfig
