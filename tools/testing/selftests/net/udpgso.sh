#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run a series of udpgso regression tests

set -o errexit
set -o nounset

setup_loopback() {
	ip addr add dev lo 10.0.0.1/32
	ip addr add dev lo fd00::1/128 nodad noprefixroute
}

test_dev_mtu() {
	setup_loopback
	# Reduce loopback MTU
	ip link set dev lo mtu 1500
}

test_route_mtu() {
	setup_loopback
	# Remove default local routes
	ip route del local 10.0.0.1/32 table local dev lo
	ip route del local fd00::1/128 table local dev lo
	# Install local routes with reduced MTU
	ip route add local 10.0.0.1/32 table local dev lo mtu 1500
	ip route add local fd00::1/128 table local dev lo mtu 1500
}

setup_dummy_sink() {
	mtu="${1:-1500}"
	prefix4="${2:-10.0.0.2/24}"
	prefix6="${3:-fd00::2/48}"

	ip link add name sink mtu "${mtu}" type dummy
	ip addr add dev sink "${prefix4}"
	ip addr add dev sink "${prefix6}" nodad
	ip link set dev sink up
}

test_hw_gso_hw_csum() {
	setup_dummy_sink
	ethtool -K sink tx-checksum-ip-generic on >/dev/null
	ethtool -K sink tx-udp-segmentation on >/dev/null
}

test_sw_gso_hw_csum() {
	setup_dummy_sink
	ethtool -K sink tx-checksum-ip-generic on >/dev/null
	ethtool -K sink tx-udp-segmentation off >/dev/null
}

test_sw_gso_sw_csum() {
	setup_dummy_sink
	ethtool -K sink tx-checksum-ip-generic off >/dev/null
	ethtool -K sink tx-udp-segmentation off >/dev/null
}

setup_ipip_tunnel() {
	setup_dummy_sink 1520 10.1.1.2/24 fd11::2/48

	ip tunnel add iptnl mode ipip local 10.1.1.2 remote 10.1.1.1
	ip addr add dev iptnl 10.0.0.2/24
	ip addr add dev iptnl fd00::2/48 nodad
	ip link set dev iptnl up
}

test_tunnel_hw_csum() {
	setup_ipip_tunnel
	ethtool -K iptnl tx-checksum-ip-generic on >/dev/null
}

test_tunnel_sw_csum() {
	setup_ipip_tunnel
	ethtool -K iptnl tx-checksum-ip-generic off >/dev/null
}

if [ "$#" -gt 0 ]; then
	"$1"
	shift 2 # pop "test_*" arg and "--" delimiter
	exec "$@"
fi

echo "ipv4 cmsg"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -4 -C

echo "ipv4 setsockopt"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -4 -C -s

echo "ipv6 cmsg"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -6 -C

echo "ipv6 setsockopt"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -6 -C -s

echo "ipv4 connected"
./in_netns.sh "$0" test_route_mtu -- ./udpgso -4 -c

echo "ipv6 connected"
./in_netns.sh "$0" test_route_mtu -- ./udpgso -6 -c

echo "ipv4 msg_more"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -4 -C -m

echo "ipv6 msg_more"
./in_netns.sh "$0" test_dev_mtu -- ./udpgso -6 -C -m

echo "ipv4 hw-gso hw-csum"
./in_netns.sh "$0" test_hw_gso_hw_csum -- ./udpgso -4 -C -R

echo "ipv6 hw-gso hw-csum"
./in_netns.sh "$0" test_hw_gso_hw_csum -- ./udpgso -6 -C -R

echo "ipv4 sw-gso hw-csum"
./in_netns.sh "$0" test_sw_gso_hw_csum -- ./udpgso -4 -C -R

echo "ipv6 sw-gso hw-csum"
./in_netns.sh "$0" test_sw_gso_hw_csum -- ./udpgso -6 -C -R

echo "ipv4 sw-gso sw-csum"
./in_netns.sh "$0" test_sw_gso_sw_csum -- ./udpgso -4 -C -R

echo "ipv6 sw-gso sw-csum"
./in_netns.sh "$0" test_sw_gso_sw_csum -- ./udpgso -6 -C -R

echo "ipv4 tunnel hw-csum"
./in_netns.sh "$0" test_tunnel_hw_csum -- ./udpgso -4 -C -R

echo "ipv6 tunnel hw-csum"
./in_netns.sh "$0" test_tunnel_hw_csum -- ./udpgso -6 -C -R

echo "ipv4 tunnel sw-csum"
./in_netns.sh "$0" test_tunnel_sw_csum -- ./udpgso -4 -C -R

echo "ipv6 tunnel sw-csum"
./in_netns.sh "$0" test_tunnel_sw_csum -- ./udpgso -6 -C -R
