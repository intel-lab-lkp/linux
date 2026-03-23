#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test the firmware_class.search= module parameter, which allows
# specifying multiple colon-separated firmware search directories.

set -e

TEST_REQS_FW_SYSFS_FALLBACK="no"
TEST_REQS_FW_SET_CUSTOM_PATH="no"
TEST_DIR=$(dirname $0)
source $TEST_DIR/fw_lib.sh

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

SEARCH_SYSFS="/sys/module/firmware_class/parameters/search"
PATH_SYSFS="/sys/module/firmware_class/parameters/path"

check_mods
check_setup
verify_reqs

if [ ! -f "$SEARCH_SYSFS" ]; then
	echo "$0: search= parameter not available, skipping"
	exit $ksft_skip
fi

# Save original values
OLD_SEARCH="$(cat $SEARCH_SYSFS)"
OLD_PATH="$(cat $PATH_SYSFS)"

# Create temp directories for firmware
FWDIR1=$(mktemp -d)
FWDIR2=$(mktemp -d)
FWDIR3=$(mktemp -d)

FW_NAME="test-search-fw.bin"
FW_CONTENT1="SEARCH_PATH_1"
FW_CONTENT2="SEARCH_PATH_2"
FW_CONTENT3="SEARCH_PATH_3"

DIR=/sys/devices/virtual/misc/test_firmware

cleanup()
{
	# Restore original values
	if [ "$OLD_PATH" = "" ]; then
		printf '\000' >$PATH_SYSFS
	else
		echo -n "$OLD_PATH" >$PATH_SYSFS
	fi
	if [ "$OLD_SEARCH" = "" ]; then
		printf '\000' >$SEARCH_SYSFS
	else
		echo -n "$OLD_SEARCH" >$SEARCH_SYSFS
	fi
	rm -rf "$FWDIR1" "$FWDIR2" "$FWDIR3"
}
trap cleanup EXIT

# Clear path= so search= is consulted
printf '\000' >$PATH_SYSFS

# Test 1: firmware found in first search path
echo -n "$FW_CONTENT1" >"$FWDIR1/$FW_NAME"
echo -n "$FWDIR1:$FWDIR2" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 1" >&2
	exit 1
fi
if ! diff -q "$FWDIR1/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 1" >&2
	exit 1
fi
echo "$0: search path - first directory: OK"

# Test 2: firmware found in second search path (not in first)
rm -f "$FWDIR1/$FW_NAME"
echo -n "$FW_CONTENT2" >"$FWDIR2/$FW_NAME"
echo -n "$FWDIR1:$FWDIR2" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 2" >&2
	exit 1
fi
if ! diff -q "$FWDIR2/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 2" >&2
	exit 1
fi
echo "$0: search path - second directory: OK"

# Test 3: firmware not found in any search path
rm -f "$FWDIR2/$FW_NAME"
echo -n "$FWDIR1:$FWDIR2" >$SEARCH_SYSFS

if echo -n "nonexistent-$FW_NAME" >$DIR/trigger_request 2>/dev/null; then
	echo "$0: FAIL - firmware should not have been found in test 3" >&2
	exit 1
fi
echo "$0: search path - not found: OK"

# Test 4: path= takes priority over search=
echo -n "$FW_CONTENT1" >"$FWDIR1/$FW_NAME"
echo -n "$FW_CONTENT2" >"$FWDIR2/$FW_NAME"
echo -n "$FWDIR1" >$PATH_SYSFS
echo -n "$FWDIR2" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 4" >&2
	exit 1
fi
if ! diff -q "$FWDIR1/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - path= should take priority over search= in test 4" >&2
	exit 1
fi
echo "$0: search path - path= priority over search=: OK"

# Clear path= again for remaining tests
printf '\000' >$PATH_SYSFS

# Test 5: three search paths, firmware in third
rm -f "$FWDIR1/$FW_NAME" "$FWDIR2/$FW_NAME"
echo -n "$FW_CONTENT3" >"$FWDIR3/$FW_NAME"
echo -n "$FWDIR1:$FWDIR2:$FWDIR3" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 5" >&2
	exit 1
fi
if ! diff -q "$FWDIR3/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 5" >&2
	exit 1
fi
echo "$0: search path - third directory: OK"

# Test 6: empty search= should not break anything
rm -f "$FWDIR3/$FW_NAME"
printf '\000' >$SEARCH_SYSFS

if echo -n "nonexistent-$FW_NAME" >$DIR/trigger_request 2>/dev/null; then
	echo "$0: FAIL - empty search= should not find firmware" >&2
	exit 1
fi
echo "$0: search path - empty search=: OK"

# Test 7: verify sysfs readback matches what was written
echo -n "$FWDIR1:$FWDIR2:$FWDIR3" >$SEARCH_SYSFS
READBACK="$(cat $SEARCH_SYSFS)"
EXPECTED="$FWDIR1:$FWDIR2:$FWDIR3"
if [ "$READBACK" != "$EXPECTED" ]; then
	echo "$0: FAIL - sysfs readback mismatch: '$READBACK' != '$EXPECTED'" >&2
	exit 1
fi
echo "$0: search path - sysfs readback: OK"

# Test 8: escaped colon in directory name (\: -> literal ':')
FWDIR_COLON=$(mktemp -d)/fw:dir
mkdir -p "$FWDIR_COLON"
echo -n "$FW_CONTENT1" >"$FWDIR_COLON/$FW_NAME"
# Write the path with the colon escaped as \:
ESCAPED_COLON="${FWDIR_COLON//:/\\:}"
echo -n "$ESCAPED_COLON" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 8" >&2
	rm -rf "$(dirname "$FWDIR_COLON")"
	exit 1
fi
if ! diff -q "$FWDIR_COLON/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 8" >&2
	rm -rf "$(dirname "$FWDIR_COLON")"
	exit 1
fi
rm -rf "$(dirname "$FWDIR_COLON")"
echo "$0: search path - escaped colon in directory: OK"

# Test 9: escaped backslash in directory name (\\ -> literal '\')
FWDIR_BS=$(mktemp -d)/fw\\dir
mkdir -p "$FWDIR_BS"
echo -n "$FW_CONTENT2" >"$FWDIR_BS/$FW_NAME"
# Write the path with backslashes escaped as \\
ESCAPED_BS="${FWDIR_BS//\\/\\\\}"
echo -n "$ESCAPED_BS" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 9" >&2
	rm -rf "$(dirname "$FWDIR_BS")"
	exit 1
fi
if ! diff -q "$FWDIR_BS/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 9" >&2
	rm -rf "$(dirname "$FWDIR_BS")"
	exit 1
fi
rm -rf "$(dirname "$FWDIR_BS")"
echo "$0: search path - escaped backslash in directory: OK"

# Test 10: escaped colon with multiple search paths
FWDIR_COLON2=$(mktemp -d)/has:colon
mkdir -p "$FWDIR_COLON2"
echo -n "$FW_CONTENT3" >"$FWDIR_COLON2/$FW_NAME"
ESCAPED_COLON2="${FWDIR_COLON2//:/\\:}"
# First path is normal (no firmware), second has escaped colon (has firmware)
echo -n "$FWDIR1:$ESCAPED_COLON2" >$SEARCH_SYSFS

if ! echo -n "$FW_NAME" >$DIR/trigger_request; then
	echo "$0: FAIL - could not trigger request for test 10" >&2
	rm -rf "$(dirname "$FWDIR_COLON2")"
	exit 1
fi
if ! diff -q "$FWDIR_COLON2/$FW_NAME" /dev/test_firmware >/dev/null; then
	echo "$0: FAIL - firmware content mismatch in test 10" >&2
	rm -rf "$(dirname "$FWDIR_COLON2")"
	exit 1
fi
rm -rf "$(dirname "$FWDIR_COLON2")"
echo "$0: search path - escaped colon with multiple paths: OK"

echo "$0: all search path tests passed"
exit 0
