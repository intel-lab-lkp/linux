#!/bin/bash -eu
# SPDX-License-Identifier: GPL-2.0
#
# Author: Brett A C Sheffield <bacs@librecast.net>
#
# Ensure destination ethernet field is correctly set for
# broadcast packets

if ! which tcpdump > /dev/null 2>&1; then
        echo "No tcpdump found. Required for this test."
        exit $ERR
fi

CAPFILE=$(mktemp -u cap.XXXXXXXXXX)

# start tcpdump listening on udp port 9
# tcpdump will exit after receiving a single packet
# timeout will kill tcpdump if it is still running after 2s
timeout 2s tcpdump -c 1 -w ${CAPFILE} udp port 9 > /dev/null 2>&1 &
PID=$!
sleep 0.1 # let tcpdump wake up

echo "Testing ethernet broadcast destination"

# send broadcast UDP packet to port 9 (DISCARD)
echo "Alonso is a good boy" | socat - udp-datagram:255.255.255.255:9,broadcast

# wait for tcpdump for exit after receiving packet
wait $PID

# compare ethernet destination field to ff:ff:ff:ff:ff:ff
# pcap has a 24 octet header + 16 octet header for each packet
# ethernet destination is the first field in the packet
printf '\xff\xff\xff\xff\xff\xff'| cmp -i40:0 -n6 ${CAPFILE} > /dev/null 2>&1
RESULT=$?

rm -f "${CAPFILE}"
exit $RESULT
