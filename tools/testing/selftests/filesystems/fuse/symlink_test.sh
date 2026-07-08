#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

exit_cleanup()
{
	fusermount -u ./mnt
	rmdir ./mnt
}

assert_value ()
{
	if [ $1 -ne $2 ]; then
		echo "FAILED"
		echo $3
		exit 1
	fi
}

set -e

trap exit_cleanup EXIT

mkdir -p mnt

./symlink_fs ./mnt

echo -n "Testing symlink without cached: "

# When symlink caching is disabled every access to a symlink is expected to
# result in a call to user-space
for i in $(seq 1 10); do
	readlink ./mnt/link > /dev/null
done

res=$(cat ./mnt/file)
assert_value $res $i "Got $res, expected $i"
echo "PASSED"

fusermount -u ./mnt

./symlink_fs --cache ./mnt

echo -n "Testing symlink with cache: "

# With caching enabled, there will only be a single call into user-space
for i in $(seq 0 10); do
	readlink ./mnt/link > /dev/null
done

res=$(cat ./mnt/file)
assert_value 1 $res "Got $ res, expected 1"
echo "PASSED"
