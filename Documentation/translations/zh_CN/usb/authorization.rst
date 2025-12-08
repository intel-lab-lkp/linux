.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/usb/authorization.rst
:翻译:

 白钶凡 Kefan Bai <baikefan@leap-io-kernel.com>

:校译:


===========================
授权或禁止USB设备连接到系统
===========================

版权 (C) 2007 Inaky Perez-Gonzalez <inaky@linux.intel.com> 英特尔公司

此功能允许你控制系统中USB设备的使用权限。
你可以借此实现USB设备的锁定，并由用户空间完全控制。

目前为止，当插入一个USB设备时，系统会配置该USB设备，其接口会立即对用户开放。
通过此修改，只有在root授权配置设备后，用户才能使用它。


使用方法
=========

授权设备连接::

	$ echo 1 > /sys/bus/usb/devices/DEVICE/authorized

禁止设备连接::
	$ echo 0 > /sys/bus/usb/devices/DEVICE/authorized

将新连接到hostX的设备默认设置为未授权（即：锁定）::

	$ echo 0 > /sys/bus/usb/devices/usbX/authorized_default

解除锁定::

	$ echo 1 > /sys/bus/usb/devices/usbX/authorized_default

默认情况下，所有USB设备都是授权的。
向authorized_default属性写入 "2" 会使内核默认只授权连接到内部USB端口的设备。

系统锁定示例（简单示例）
------------------------

假设你想实现一个锁定功能，要求只有类型为XYZ的设备可以连接
（例如，它是一个带有可见USB端口的自助服务终端）::

  启动系统
  rc.local ->

   for host in /sys/bus/usb/devices/usb*
   do
      echo 0 > $host/authorized_default
   done

将一个脚本挂接到udev，当插入新的USB设备时，该脚本就会被自动触发::

 if device_is_my_type $DEV
 then
   echo 1 > $device_path/authorized
 done


这里的device_is_my_type()就是实现锁定的关键所在。
仅仅检查class、type 和protocol是否匹配某个值，
是最差的安全验证方式（但对于想要破解的人却是最容易的）。
如果你需要真正安全的方案，应使用加密、证书认证等手段。
一个针对存储密钥的简单示例::

 function device_is_my_type()
 {
   echo 1 > authorized		# 暂时授权它
                                # FIXME: 确保没有人能够挂载它
   mount DEVICENODE /mntpoint
   sum=$(md5sum /mntpoint/.signature)
   if [ $sum = $(cat /etc/lockdown/keysum) ]
   then
        echo "We are good, connected"
        umount /mntpoint
        # 添加一些额外的内容，以便其他人也可以使用它
   else
        echo 0 > authorized
   fi
 }


当然，这种做法很简陋；实际上你应该使用基于PKI的真正证书验证，
这样就不会依赖共享密钥之类的东西。不过你明白我的意思。
任何拿到设备仿真工具包的人都能伪造描述符和设备信息。
所以千万不要信任这些信息。

接口授权
--------

也有类似的方法用于允许或拒绝特定USB接口。这允许只阻止USB设备的一个子集。

授权接口::

	$ echo 1 > /sys/bus/usb/devices/INTERFACE/authorized

取消授权接口::

	$ echo 0 > /sys/bus/usb/devices/INTERFACE/authorized

也可以更改新接口在特定USB总线上的默认值。

默认允许接口::

	$ echo 1 > /sys/bus/usb/devices/usbX/interface_authorized_default

默认拒绝接口::
	$ echo 0 > /sys/bus/usb/devices/usbX/interface_authorized_default

默认情况下，interface_authorized_default位为1。
因此，所有接口默认都是授权的。

注意：
  如果要对一个未授权的接口进行授权，则必须通过将INTERFACE写入
  /sys/bus/usb/drivers_probe来手动触发驱动程序进行探测。
  对于使用多个接口的驱动程序，需要先对所有使用的接口进行授权。
  之后应探测驱动程序。这样做可以避免副作用。
