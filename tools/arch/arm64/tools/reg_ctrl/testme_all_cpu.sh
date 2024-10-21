#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

rmmod reg_ctrl.ko
insmod reg_ctrl.ko
NR_CORES=16

i=0
while [ $i -lt $NR_CORES ]
do
	echo "run on core: $i"
	taskset -c $i cat /sys/kernel/reg_ctrl/system/control/VNCR_EL2
	let "i=i+1"
	echo ""
done
