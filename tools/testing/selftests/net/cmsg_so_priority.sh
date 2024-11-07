#!/bin/bash

source lib.sh

IP4=192.168.0.2/16
TGT4=192.168.0.3/16
TGT4_NO_MASK=192.168.0.3
IP6=2001:db8::2/64
TGT6=2001:db8::3/64
TGT6_NO_MASK=2001:db8::3
IP4BR=192.168.0.1/16
IP6BR=2001:db8::1/64
PORT=8080
DELAY=400000
QUEUE_NUM=4


cleanup() {
    ip netns del red 2>/dev/null
    ip netns del green 2>/dev/null
    ip link del br0 2>/dev/null
    ip link del vethcab0 2>/dev/null
    ip link del vethcab1 2>/dev/null
}

trap cleanup EXIT

priority_values=($(seq 0 $((QUEUE_NUM - 1))))

queue_config=""
for ((i=0; i<$QUEUE_NUM; i++)); do
    queue_config+=" 1@$i"
done

map_config=$(seq 0 $((QUEUE_NUM - 1)) | tr '\n' ' ')

ip netns add red
ip netns add green
ip link add br0 type bridge
ip link set br0 up
ip addr add $IP4BR dev br0
ip addr add $IP6BR dev br0

ip link add vethcab0 type veth peer name red0
ip link set vethcab0 master br0
ip link set red0 netns red
ip netns exec red bash -c "
ip link set lo up
ip link set red0 up
ip addr add $IP4 dev red0
ip addr add $IP6 dev red0
sysctl -w net.ipv4.ping_group_range='0 2147483647'
exit"
ip link set vethcab0 up

ip link add vethcab1 type veth peer name green0
ip link set vethcab1 master br0
ip link set green0 netns green
ip netns exec green bash -c "
ip link set lo up
ip link set green0 up
ip addr add $TGT4 dev green0
ip addr add $TGT6 dev green0
exit"
ip link set vethcab1 up

ip netns exec red bash -c "
sudo ethtool -L red0 tx $QUEUE_NUM rx $QUEUE_NUM
sudo tc qdisc add dev red0 root mqprio num_tc $QUEUE_NUM queues $queue_config map $map_config hw 0
exit"

get_queue_bytes() {
    ip netns exec red sudo tc -s qdisc show dev red0 | grep 'Sent' | awk '{print $2}'
}

TOTAL_TESTS=0
FAILED_TESTS=0

check_result() {
    ((TOTAL_TESTS++))
    if [ "$1" -ne 0 ]; then
        ((FAILED_TESTS++))
    fi
}


for i in 4 6; do
    [ $i == 4 ] && TGT=$TGT4_NO_MASK || TGT=$TGT6_NO_MASK

    for p in u i r; do
        echo "Test IPV$i, prot: $p"
        for value in "${priority_values[@]}"; do
            ip netns exec red ./cmsg_sender -$i -Q $value -d "${DELAY}" -p $p $TGT $PORT
            setsockopt_priority_bytes_num=($(get_queue_bytes))

            ip netns exec red ./cmsg_sender -$i -P $value -d "${DELAY}" -p $p $TGT $PORT
            cmsg_priority_bytes_num=($(get_queue_bytes))

            if [[ "${cmsg_priority_bytes_num[$actual_queue]}" != \
            "${setsockopt_priority_bytes_num[$actual_queue]}" ]]; then
                check_result 0
            else
                check_result 1
            fi
        done
    done
done

if [ $FAILED_TESTS -ne 0 ]; then
    echo "FAIL - $FAILED_TESTS/$TOTAL_TESTS tests failed"
    exit 1
else
    echo "OK - All $TOTAL_TESTS tests passed"
    exit 0
fi
