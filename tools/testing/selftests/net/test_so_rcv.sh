#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

HOST=127.0.0.1
PORT=1234
TOTAL_TESTS=0
FAILED_TESTS=0

declare -A TESTS=(
	["SO_RCVPRIORITY"]="-P 2"
	["SO_RCVMARK"]="-M 3"
)

check_result() {
	((TOTAL_TESTS++))
	if [ "$1" -ne 0 ]; then
		((FAILED_TESTS++))
	fi
}

for test_name in "${!TESTS[@]}"; do
	echo "Running $test_name test"
	arg=${TESTS[$test_name]}

	./so_rcv_listener $arg $HOST $PORT &
	LISTENER_PID=$!

	if ./cmsg_sender $arg $HOST $PORT; then
		echo "Sender succeeded for $test_name"
	else
		echo "Sender failed for $test_name"
		kill "$LISTENER_PID" 2>/dev/null
		wait "$LISTENER_PID"
		check_result 1
		continue
	fi

	wait "$LISTENER_PID"
	LISTENER_EXIT_CODE=$?

	if [ "$LISTENER_EXIT_CODE" -eq 0 ]; then
		echo "Rcv test OK for $test_name"
		check_result 0
	else
		echo "Rcv test FAILED for $test_name"
		check_result 1
	fi
done

if [ "$FAILED_TESTS" -ne 0 ]; then
	echo "FAIL - $FAILED_TESTS/$TOTAL_TESTS tests failed"
	exit 1
else
	echo "OK - All $TOTAL_TESTS tests passed"
	exit 0
fi
