#!/bin/bash
# SPDX-License-Identifier: MIT

set -euxo pipefail

: "${KERNEL_ARCH:?ERROR: KERNEL_ARCH must be set}"

./tools/testing/kunit/kunit.py run \
  --arch "${KERNEL_ARCH}" \
  --make_option LLVM=1 \
  --kunitconfig=drivers/gpu/drm/tests
