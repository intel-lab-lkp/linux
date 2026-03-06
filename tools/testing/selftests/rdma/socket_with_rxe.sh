#!/bin/sh

# Notes:
#
# 1. Before running this script, please disable the firewall, as it may
# block UDP port 4791.

# 2. This test script depends on the veth and tun drivers. Before running
#  the script, please verify that both drivers are available by executing:
#
# modinfo tun
#
# Make sure these commands return valid module information.

# Check if socket exist or not
exec > /dev/null
ip tuntap add mode tun tun0
ip -4 a
ip addr add 1.1.1.1/24 dev tun0
ip link set tun0 up
ip -4 a
rdma link add rxe0 type rxe netdev tun0
rdma link
ret=`ss -lun | grep :4791`
if [ X"$ret" == X"" ]; then
	echo "Error: udp port 4791 does not exist"
	exit 1
fi

ip tuntap add mode tun tun1
ip -4 a
ip addr add 2.2.2.2/24 dev tun1
ip link set tun1 up
rdma link add rxe1 type rxe netdev tun1
rdma link
ret=`ss -lun | grep :4791`
if [ X"$ret" == X"" ]; then
	echo "Error: udp port 4791 does not exist"
	exit 1
fi

rdma link del rxe1
rdma link
ret=`ss -lun | grep :4791`
if [ X"$ret" == X"" ]; then
	echo "Error: udp port 4791 doese not exist"
	exit 1
fi

rdma link del rxe0
rdma link
if ss -lun | grep :4791; then
	echo "Error: udp port 4791 exists"
	exit 1
fi

ip addr del 2.2.2.2/24 dev tun1
ip tuntap del mode tun tun1

ip addr del 1.1.1.1/24 dev tun0
ip tuntap del mode tun tun0

modprobe -v -r tun
modprobe -v -r rdma_rxe
