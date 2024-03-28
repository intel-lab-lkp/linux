# SPDX-License-Identifier: GPL-2.0-only
PY3 = $(shell which python3 2>/dev/null)

ifneq ($(PY3),)

TEST_PROGS := test.py dm_zero.sh dm_thin.sh

include ../lib.mk

else

all: no_py3_warning

no_py3_warning:
	@echo "Missing python3. This test will be skipped."

endif
