#!/bin/bash -e

for sc in {r,w}mem_{default,max}; do
	# change the value in the host netns
	sysctl -qw "net.core.$sc=300000"

	# check that the value is read from the init netns
	[ "$(unshare -n sysctl -n "net.core.$sc")" -eq 300000 ]

	# check that this isn't writeable in a netns
	! unshare -n [ -w "/proc/sys/net/core/$sc" ]
	! unshare -n sysctl -w "net.core.$sc=100000"
done

echo 'Test passed OK'
