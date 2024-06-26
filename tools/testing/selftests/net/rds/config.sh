#! /bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e
set -u
set -x

unset KBUILD_OUTPUT

GENERATE_GCOV_REPORT=0
while getopts "g" opt; do
  case ${opt} in
    g)
      GENERATE_GCOV_REPORT=1
      ;;
    :)
      echo "USAGE: config.sh [-g]"
      exit 1
      ;;
    ?)
      echo "Invalid option: -${OPTARG}."
      exit 1
      ;;
  esac
done

# start with a default config
make defconfig

# no modules
scripts/config --disable CONFIG_MODULES

# enable RDS
scripts/config --enable CONFIG_RDS
scripts/config --enable CONFIG_RDS_TCP

if [ "$GENERATE_GCOV_REPORT" -eq 1 ]; then
	# instrument RDS and only RDS
	scripts/config --enable CONFIG_GCOV_KERNEL
	scripts/config --disable GCOV_PROFILE_ALL
	scripts/config --enable GCOV_PROFILE_RDS
else
	scripts/config --disable CONFIG_GCOV_KERNEL
	scripts/config --disable GCOV_PROFILE_ALL
	scripts/config --disable GCOV_PROFILE_RDS
fi

# need network namespaces to run tests with veth network interfaces
scripts/config --enable CONFIG_NET_NS
scripts/config --enable CONFIG_VETH

# simulate packet loss
scripts/config --enable CONFIG_NET_SCH_NETEM

# generate real .config without asking any questions
make olddefconfig
