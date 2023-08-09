#!/bin/bash
# perf script gecko test
# SPDX-License-Identifier: GPL-2.0

err=0

cleanup() {
  rm -rf gecko_profile.json
  trap - exit term int
}

trap_cleanup() {
  cleanup
  exit 1
}
trap trap_cleanup exit term int

report() {
    if [ "$1" = 0 ]; then
        echo "PASS: \"$2\""
    else
        echo "FAIL: \"$2\" Error message: \"$3\""
        err=1
    fi
}

find_str_or_fail() {
    grep -q "$1" <<< "$2"
    if [ "$?" != 0 ]; then
        report 1 "$3" "Failed to find required string:'${1}'."
    else
        report 0 "$3"
    fi
}

# To validate the json format, check if python is installed
if [ "$PYTHON" = "" ] ; then
	if which python3 > /dev/null ; then
		PYTHON=python3
	elif which python > /dev/null ; then
		PYTHON=python
	else
		echo Skipping JSON format check, python not detected please set environment variable PYTHON.
		PYTHON_NOT_AVAILABLE=1
	fi
fi

# Check execution of perf script gecko command
test_gecko_command() {
    echo "Testing Gecko Command"
    perf script gecko -a sleep 0.5
	# Store the content of the file in the 'out' variable
    out=$(< "gecko_profile.json")
	# Get the length of the gecko_profile.json output in 'out'
	length=${#out}
	if [ "$length" -gt 0 ]; then
        echo "PASS: \"Gecko Command\""
    else
        echo "FAIL: \"Gecko Command\""
        err=1
        exit
    fi
}

# with the help of python json libary validate the json output
if [ "$PYTHON_NOT_AVAILABLE" != "0" ]; then
	validate_json_format()
	{
		if [ "$out" ] ; then
			if [ "$PYTHON -c import json; json.load($out)" ]; then
				echo "PASS: \"The file contains valid JSON format\""
			else
				echo "FAIL: \"The file does not contain valid JSON format\""
				err=1
				exit
			fi
		else
			echo "FAIL: \"File not found\""
			err=2
			exit
		fi
	}
fi

# validate output for the presence of "meta".
test_meta() {
    find_str_or_fail "meta" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "threads".
test_threads() {
	find_str_or_fail "threads" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "samples".
test_samples() {
	find_str_or_fail "samples" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "frameTable".
test_frametable() {
	find_str_or_fail "frameTable" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "stackTable".
test_stacktable() {
	find_str_or_fail "stackTable" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "stringTable"
test_stringtable() {
	find_str_or_fail "stringTable" "$out" "${FUNCNAME[0]}"
}

# validate output for the presence of "pausedRanges".
test_pauseranges(){
	find_str_or_fail "pausedRanges" "$out" "${FUNCNAME[0]}"
}

prepare_perf_data
test_gecko_command
validate_json_format
test_meta
test_threads
test_samples
test_frametable
test_stacktable
test_stringtable
test_pauseranges
cleanup
exit $err
