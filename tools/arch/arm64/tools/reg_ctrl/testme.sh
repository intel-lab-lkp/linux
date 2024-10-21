#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
rmmod reg_ctrl.ko
insmod reg_ctrl.ko

echo "start test read, expect values:"
echo "
0x0
0x4000240340543001
0x0
0x2000315a10000045
0x7000336f
0x15401480136
0x70300
"
cat /sys/kernel/reg_ctrl/system/implementation_defined/IMP_CPU*

