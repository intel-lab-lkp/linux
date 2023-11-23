#!/usr/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Wrapper script for running the DPLL system integration tests.
#
# The script check if all the requirements are fulfilled before running pytest.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

ENOPKG=65  # Package not installed
TEMP_VENV=$(mktemp -u)
KSRC=${KSRC:-$(git rev-parse --show-toplevel)}
PYTHON=${PYTHON:-python3}

cleanup() {
    [ -n "$VIRTUAL_ENV" ] && deactivate

    if [[ -d "$TEMP_VENV" ]]; then
        echo "Removing temporary virtual environment ($TEMP_VENV)"
        rm -r "$TEMP_VENV"
    else
        echo "Temporary virtual environment does not exist"
    fi
}

skip () {
    cleanup
    echo "SKIP: $1"
    exit $2
}

# 1) To run tests, we need Python 3 installed
which $PYTHON 2>&1 1> /dev/null
if [[ $? -ne 0 ]]; then
    skip "Python 3 is not installed" $ENOPKG
fi

# 2) ...at least Python 3.7 (2018)
$PYTHON -c "import sys;vi=sys.version_info;
sys.exit(0) if vi[0] == 3 and vi[1] >= 7 else sys.exit(1)"
if [[ $? -ne 0 ]]; then
    skip "At least Python 3.7 is required (set PYTHON for custom path)" $ENOPKG
fi

# 3) Let's make sure we have predictable environment (virtual environment)
#   a) Create venv
$PYTHON -m venv $TEMP_VENV
if [[ $? -ne 0 ]]; then
    skip "Could not create virtual environment" $ENOPKG
fi

#   b) Activate venv
source $TEMP_VENV/bin/activate
if [[ $? -ne 0 ]]; then
    skip "Could not activate the virtual environment" $ENOPKG
fi

#   c) Install the exact packages versions we need
pip install -r requirements.txt
if [[ $? -ne 0 ]]; then
    skip "Could not install the required packages" $ENOPKG
fi

# 4) Finally, run the tests!
KSRC=$KSRC pytest $PYTEST_PARAMS
result=$?
if [[ $result -ne 0 ]]; then
    echo "ERROR: Some of the DPLL tests failed"
fi

# 5) Clean up after execution
cleanup

exit $result
