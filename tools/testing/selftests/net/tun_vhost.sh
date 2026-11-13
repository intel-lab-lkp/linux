#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run tun_vhost with vhost_net loaded, and once more with zerocopy enabled
# if the module can be reloaded.

ksft_skip=4
param=/sys/module/vhost_net/parameters/experimental_zcopytx

modprobe -q vhost_net
[ -e /dev/vhost-net ] || exit $ksft_skip

./tun_vhost
ret=$?

zc=$(cat "$param" 2>/dev/null)
if [ "$zc" = 0 ] && modprobe -q -r vhost_net 2>/dev/null; then
	modprobe vhost_net experimental_zcopytx=1 &&
		{ ./tun_vhost -v zerocopy || ret=1; }
	modprobe -q -r vhost_net
	modprobe vhost_net experimental_zcopytx="$zc"
fi
exit $ret
