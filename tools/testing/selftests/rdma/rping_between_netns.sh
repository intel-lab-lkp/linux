#!/bin/sh

# Notes:
#
# 1. Before running this script, please disable the firewall, as it may
# block UDP port 4791.

# 2. This test script depends on the veth and tun drivers. Before running
#  the script, please verify that both drivers are available by executing:
#
# modinfo veth
#
# Make sure these commands return valid module information.

#1. Check if rping can work or not
exec > /dev/null
ip netns add test1
ip netns ls
ip link add veth-a type veth peer name veth-b
ip l
ip link set veth-a netns test1
ip l
ip netns exec test1 ip l set veth-a up
ip netns exec test1 ip addr add 1.1.1.1/24 dev veth-a
ip netns exec test1 ip l
ip netns exec test1 ip -4 a
ip netns exec test1 rdma link add rxe0 type rxe netdev veth-a

#check if socket exist or not
ip netns exec test1 ss -lun | grep :4791

ip netns exec test1 rdma link
ip link set veth-b up
ip addr add 1.1.1.2/24 dev veth-b
ping -c 3 1.1.1.1 || exit 1
ip netns exec test1 rping -s -a 1.1.1.1&
rdma link add rxe1 type rxe netdev veth-b
rdma link

#check if socket exist or not
ss -lun | grep :4791

rping -c -a 1.1.1.1 -d -v -C 3 || exit 1
ip netns ls
rdma link del rxe1

#check if socket exist or not
ss -lun | grep :4791

ip netns exec test1 ss -lun | grep :4791
ip netns exec test1 rdma link del rxe0
ip netns exec test1 ss -lun | grep :4791
ip netns del test1
ip netns ls

modprobe -v -r veth
modprobe -v -r rdma_rxe
