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

#sudo su -
#cd /home/miguel/kernel/linux/tools/testing/selftests/filesystems/fuse
#dmesg -c > /dev/null
#tmux -S ./bla -f a
#./acl_fs -d --cache ./mnt
#setfacl -m u:daemon:r mnt/file
#getfacl mnt/file

./acl_fs ./mnt

echo -n "Testing ACLs without cache: "

# set ACL
./setgetacl ACCESS u::rw-,g::rw-,o::rw-,u:nobody:r--,m::rw- mnt/file

# When symlink caching is disabled every access to a symlink is expected to
# result in a call to user-space
for i in $(seq 1 10); do
	./setgetacl ACCESS mnt/file > /dev/null
done

res=$(cat ./mnt/file)
assert_value $res $i "Got $res, expected $i"
echo "PASSED"

fusermount -u ./mnt

./acl_fs --cache ./mnt

echo -n "Testing ACLs with cache: "

# set ACL
./setgetacl ACCESS u::rw-,g::rw-,o::rw-,u:nobody:r--,m::rw- mnt/file

# With caching enabled, there will be a single call into user-space
for i in $(seq 1 10); do
	./setgetacl ACCESS mnt/file > /dev/null
done
res=$(cat ./mnt/file)
assert_value $res 1 "Got $res, expected 1"

echo "PASSED"
