#!/bin/sh

# Notes:
#
# 1. Before running this script, please disable the firewall, as it may
# block UDP port 4791.

# 2. This test script depends on the veth and tun drivers. Before running
#  the script, please verify that both drivers are available by executing:
#
# modinfo tun
# modinfo veth
#
# Make sure these commands return valid module information.

# 3. ipv6 test.
# While RXE is conventionally deployed over IPv4, it maintains
# native support for IPv6. However, IPv6 implementations typically
# receive less validation and performance tuning in standard use cases.
exec > /dev/null
# 1) create ipv6 net namespace
ip netns add net6
ip link add veth0 type veth peer name veth1
ip link set veth1 netns net6
ip netns exec net6 ip addr add 2001:db8::1/64 dev veth1
ip netns exec net6 ip link set veth1 up

# 2) Add rdma link
ip netns exec net6 rdma link add rxe6 type rxe netdev veth1

# 3) check IPv6 UDP 4791 listening port
if ! ip netns exec net6 ss -ul6n | grep :4791; then
	echo "Error: udp port 4791 exists"
	exit 1
fi

# 4) Delete rxe link
ip netns exec net6 rdma link del rxe6
if ip netns exec net6 ss -ul6n | grep :4791; then  # result should be null
	echo "Error: udp port 4791 exists"
	exit 1
fi

# 5) delete net6
ip netns del net6

modprobe -v -r rdma_rxe
