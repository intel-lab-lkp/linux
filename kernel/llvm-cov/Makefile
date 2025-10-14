# SPDX-License-Identifier: GPL-2.0
# Disable profiling for the llvm-cov subsystem itself to avoid recursion
# and potential issues with the profiling infrastructure.
# GCOV and LLVM-COV are mutually exclusive (enforced in Kconfig).
GCOV_PROFILE		:= n
LLVM_COV_PROFILE	:= n

obj-y	+= fs.o
