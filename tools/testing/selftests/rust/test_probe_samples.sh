#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2023 Collabora Ltd
#
# This script tests whether the rust sample modules can
# be added and removed correctly.
#

DIR="$(dirname "$(readlink -f "$0")")"

source "${DIR}"/ktap_helpers.sh

rust_sample_modules=("rust_minimal" "rust_print")

KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4

ret="${KSFT_PASS}"

ktap_print_header

ktap_set_plan "${#rust_sample_modules[@]}"

for sample in "${rust_sample_modules[@]}"; do
    if ! /sbin/modprobe -n -q "$sample"; then
        ktap_test_skip "module $sample is not found in /lib/modules/$(uname -r)"
        continue
    fi

    if /sbin/modprobe -q "$sample"; then
        /sbin/modprobe -q -r "$sample"
        ktap_test_pass "$sample"
    else
        ret="${KSFT_FAIL}"
        ktap_test_fail "$sample"
    fi
done

ktap_print_totals
exit "${ret}"
