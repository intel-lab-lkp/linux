#!/bin/bash
#
# This test is for checking rtnetlink notification callpaths, and get as much
# coverage as possible.
#
# set -e

ALL_TESTS="
	kci_test_mcast_addr_notification
"

VERBOSE=0
PAUSE=no
PAUSE_ON_FAIL=no

source lib.sh

# set global exit status, but never reset nonzero one.
check_err()
{
	if [ $ret -eq 0 ]; then
		ret=$1
	fi
	[ -n "$2" ] && echo "$2"
}

run_cmd_common()
{
	local cmd="$*"
	local out
	if [ "$VERBOSE" = "1" ]; then
		echo "COMMAND: ${cmd}"
	fi
	out=$($cmd 2>&1)
	rc=$?
	if [ "$VERBOSE" = "1" -a -n "$out" ]; then
		echo "    $out"
	fi
	return $rc
}

run_cmd() {
	run_cmd_common "$@"
	rc=$?
	check_err $rc
	return $rc
}

end_test()
{
	echo "$*"
	[ "${VERBOSE}" = "1" ] && echo

	if [[ $ret -ne 0 ]] && [[ "${PAUSE_ON_FAIL}" = "yes" ]]; then
		echo "Hit enter to continue"
		read a
	fi;

	if [ "${PAUSE}" = "yes" ]; then
		echo "Hit enter to continue"
		read a
	fi

}

kci_test_mcast_addr_notification()
{
	local tmpfile
	local monitor_pid
	local match_result

	tmpfile=$(mktemp)

	ip monitor maddr > $tmpfile &
	monitor_pid=$!
	sleep 1
	if [ ! -e "/proc/$monitor_pid" ]; then
		end_test "SKIP: mcast addr notification: iproute2 too old"
		rm $tmpfile
		return $ksft_skip
	fi

	run_cmd ip link add name test-dummy1 type dummy
	run_cmd ip link set test-dummy1 up
	run_cmd ip link del dev test-dummy1
	sleep 1

	match_result=$(grep -cE "test-dummy1.*(224.0.0.1|ff02::1)" $tmpfile)

	kill $monitor_pid
	rm $tmpfile
	# There should be 4 line matches as follows.
	# 13: test-dummy1    inet6 mcast ff02::1 scope global 
	# 13: test-dummy1    inet mcast 224.0.0.1 scope global 
	# Deleted 13: test-dummy1    inet mcast 224.0.0.1 scope global 
	# Deleted 13: test-dummy1    inet6 mcast ff02::1 scope global 
	if [ $match_result -ne 4 ];then
		end_test "FAIL: mcast addr notification"
		return 1
	fi
	end_test "PASS: mcast addr notification"
}

kci_test_rtnl()
{
	local current_test
	local ret=0

	for current_test in ${TESTS:-$ALL_TESTS}; do
		$current_test
		check_err $?
	done

	return $ret
}

usage()
{
	cat <<EOF
usage: ${0##*/} OPTS

        -t <test>   Test(s) to run (default: all)
                    (options: $(echo $ALL_TESTS))
        -v          Verbose mode (show commands and output)
        -P          Pause after every test
        -p          Pause after every failing test before cleanup (for debugging)
EOF
}

#check for needed privileges
if [ "$(id -u)" -ne 0 ];then
	end_test "SKIP: Need root privileges"
	exit $ksft_skip
fi

for x in ip;do
	$x -Version 2>/dev/null >/dev/null
	if [ $? -ne 0 ];then
		end_test "SKIP: Could not run test without the $x tool"
		exit $ksft_skip
	fi
done

while getopts t:hvpP o; do
	case $o in
		t) TESTS=$OPTARG;;
		v) VERBOSE=1;;
		p) PAUSE_ON_FAIL=yes;;
		P) PAUSE=yes;;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

[ $PAUSE = "yes" ] && PAUSE_ON_FAIL="no"

kci_test_rtnl

exit $?
