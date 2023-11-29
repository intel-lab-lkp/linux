#!/bin/sh
# perf list tests
# SPDX-License-Identifier: GPL-2.0

set -e
err=0

if [ "x$PYTHON" == "x" ]
then
	if which python3 > /dev/null
	then
		PYTHON=python3
	elif which python > /dev/null
	then
		PYTHON=python
	else
		echo Skipping test, python not detected please set environment variable PYTHON.
		exit 2
	fi
fi

test_list_json() {
  echo "Json output test"
  perf list -j | $PYTHON -m json.tool
  echo "Json output test [Success]"
}

test_list_json
exit $err
