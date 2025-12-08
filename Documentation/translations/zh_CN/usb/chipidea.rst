.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/usb/chipidea.rst
:翻译:

 白钶凡 Kefan Bai <baikefan@leap-io-kernel.com>

:校译:



=============================
ChipIdea高速双角色控制器驱动
=============================

1. 如何测试OTG FSM（HNP 和 SRP）
--------------------------------

接下来我们在两块Freescale i.MX6Q Sabre SD开发板上，演示如何通过sys输入文件
来测试OTG的HNP和SRP功能。

1.1 如何使能OTG FSM
--------------------

1.1.1 在menuconfig中选择CONFIG_USB_OTG_FSM，并重新编译内核
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

重新编译镜像和模块。如果需要查看OTG FSM的内部变量，可以挂载debugfs，
会有两个文件用于显示OTG FSM变量和部分控制器寄存器值::

	cat /sys/kernel/debug/ci_hdrc.0/otg
	cat /sys/kernel/debug/ci_hdrc.0/registers

1.1.2 在控制器节点的dts文件中添加以下条目
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

	otg-rev = <0x0200>;
	adp-disable;

1.2 测试步骤
-------------

1) 给两块Freescale i.MX6Q Sabre SD开发板上电，
   并加载gadget类驱动（例如 g_mass_storage）。

2) 用USB线连接两块开发板：一端是micro A插头，另一端是micro B插头。

   插入micro A插头的一端为A设备，它应当枚举另一端的B设备。

3) 角色切换

   在B设备上执行::

	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/b_bus_req

   B设备应切换为host并枚举A设备。

4) 把A设备切换为host

   在B设备上执行::

	echo 0 > /sys/bus/platform/devices/ci_hdrc.0/inputs/b_bus_req

   或者通过HNP轮询机制：B-Host可以检测到A外设想成为host的意愿，
   从而由A外设触发角色切换。在A设备上执行::
   或者通过HNP轮询机制：B-Host可以检测到A外设想切换为host角色的意愿，
   并通过A外设对轮询的响应来触发角色切换。
   这可以通过在A设备上执行::

	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/a_bus_req

   A设备应切换回host并枚举B设备。

5) 拔掉B设备（拔掉micro B插头），在10秒内重新插入；
   A设备应重新枚举B设备。

6) 拔掉B设备（拔掉micro B插头），在10秒后重新插入；
   A设备不应重新枚举B设备。

   若A设备想使用总线：

   在A设备上::

	echo 0 > /sys/bus/platform/devices/ci_hdrc.0/inputs/a_bus_drop
	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/a_bus_req

   若B设备想使用总线：

   在B设备上::

	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/b_bus_req

7) A设备关闭总线供电

   在A设备上::

	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/a_bus_drop

   A设备应断开与B设备的连接并关闭总线供电。

8) B设备进行SRP数据脉冲唤醒

   在B设备上::

	echo 1 > /sys/bus/platform/devices/ci_hdrc.0/inputs/b_bus_req

   A设备应恢复usb总线并枚举B设备。

1.3 参考文档
-------------
《On-The-Go and Embedded Host Supplement to the USB Revision 2.0 Specification
July 27, 2012 Revision 2.0 version 1.1a》

2. 如何使能USB作为系统唤醒源
----------------------------
下面是在imx6平台上使能USB作为系统唤醒源的示例。

2.1 使能核心控制器的唤醒功能::

	echo enabled > /sys/bus/platform/devices/ci_hdrc.0/power/wakeup

2.2 使能glue层的唤醒功能::
	echo enabled > /sys/bus/platform/devices/2184000.usb/power/wakeup

2.3 使能PHY的唤醒功能（可选）::

	echo enabled > /sys/bus/platform/devices/20c9000.usbphy/power/wakeup

2.4 使能根集线器的唤醒功能::
	echo enabled > /sys/bus/usb/devices/usb1/power/wakeup

2.5 使能相关设备的唤醒功能::

	echo enabled > /sys/bus/usb/devices/1-1/power/wakeup

如果系统只有一个USB端口，并且你希望在这个端口上使能USB唤醒功能，
你可以使用下面的脚本来使能USB唤醒功能::

	for i in $(find /sys -name wakeup | grep usb);do echo enabled > $i;done;
