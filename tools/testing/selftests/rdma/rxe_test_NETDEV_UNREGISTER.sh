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

# Trigger NETDEV_UNREGISTER
exec > /dev/null
ip tuntap add mode tun tun0
ip -4 a
ip addr add 1.1.1.1/24 dev tun0
ip link set tun0 up
ip -4 a
rdma link add rxe0 type rxe netdev tun0
rdma link
ss -lun | grep :4791

ip l
ip addr del 1.1.1.1/24 dev tun0
ip tuntap del mode tun tun0

rdma link
if ss -lun | grep :4791; then
	echo "error"
	exit 1
fi

modprobe -v -r tun
modprobe -v -r rdma_rxe
