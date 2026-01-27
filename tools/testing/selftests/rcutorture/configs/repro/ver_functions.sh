#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
#
# Torture-suite-dependent shell functions for the rest of the scripts.
#
# Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
#
# Authors: Paul E. McKenney <paulmck@kernel.org>

# per_version_boot_params bootparam-string config-file seconds
#
# Adds per-version torture-module parameters to kernels supporting them.
per_version_boot_params () {
	echo	repro.verbose=1 \
		repro.shutdown_secs=$3 \
		$1
}
