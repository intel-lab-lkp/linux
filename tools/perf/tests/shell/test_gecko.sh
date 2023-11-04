#!/bin/bash
# perf script gecko test
# SPDX-License-Identifier: GPL-2.0

err=0

cleanup() {
	rm -f gecko_test.json
	rm -f perf.data
	rm -f perf.data.old
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

prepare_perf_data() {
	perf record -F 99 -g -- perf test -w noploop > /dev/null 2>&1
	# check if perf data file got created in above step.
	if [ ! -e "perf.data" ]; then
		printf "FAIL: perf record failed to create \"perf.data\" \n"
		return 1
	fi
}

# Check execution of perf script gecko command
test_gecko_command() {
    echo "Testing Gecko Command"
    perf script report gecko --save-only=gecko_test.json > /dev/null 2>&1
	# Check if the Gecko script throws any error, and if so, consider it a failure
	if [ "$?" != 0 ]; then
		echo "FAIL: \"Gecko Command\""
		err=2
		exit
	else
		echo "PASS: \"Gecko Command\""
	fi
    # Store the content of the file in the 'result' variable
    result=$(< "gecko_test.json")
}

# with the help of python json libary validate the json output
if [ "$PYTHON_NOT_AVAILABLE" != "0" ]; then
	validate_json_format()
	{
		if [ "$result" ] ; then
			if [ "$PYTHON -c import json; json.load($result)" ]; then
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
	find_str_or_fail "meta" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "threads".
test_threads() {
	find_str_or_fail "threads" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "samples".
test_samples() {
	find_str_or_fail "samples" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "frameTable".
test_frametable() {
	find_str_or_fail "frameTable" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "stackTable".
test_stacktable() {
	find_str_or_fail "stackTable" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "stringTable"
test_stringtable() {
	find_str_or_fail "stringTable" "$result" "${FUNCNAME[0]}"
}

# validate output for the presence of "pausedRanges".
test_pauseranges(){
	find_str_or_fail "pausedRanges" "$result" "${FUNCNAME[0]}"
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
