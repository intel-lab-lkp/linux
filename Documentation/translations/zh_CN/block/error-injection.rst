.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/block/error-injection.rst

:翻译:

 Cui Shuang <imcusg@gmail.com>

==============
可配置错误注入
==============

概述
----

可配置错误注入允许针对块设备的扇区范围注入特定的块层状态码。错误可以无条件
注入，也可以按给定概率注入。

要使用可配置错误注入，必须启用 ``CONFIG_BLK_ERROR_INJECTION``。

唯一的接口是 ``error_injection`` debugfs 文件，每个已注册的 gendisk 都会
创建该文件。写入此文件用于创建或删除规则，读取则返回当前错误注入点的列表。

选项
----

以下选项指定操作：

===================	=======================================================
add			添加一条新规则
removeall		删除所有现有规则
===================	=======================================================

以下选项指定 ``add`` 操作的规则详情：

===================	=======================================================
op=<string>		此规则适用的块层操作。它使用每个 ``REQ_OP_XYZ``
			操作中的 ``XYZ``，例如 ``READ``、``WRITE`` 或
			``DISCARD``。必填。
status=<string>		要返回的状态。它使用每个 ``BLK_STS_XYZ`` 状态码
			中的 ``XYZ``，例如 ``IOERR`` 或 ``MEDIUM``。必填。
start=<number>		此规则适用的第一个块层扇区。可选，默认为 0。
nr_sectors=<number>	此规则适用的扇区数。可选，默认覆盖设备的剩余部分。
chance=<number>		仅以 ``1/chance`` 的概率返回失败。可选，默认为 1
			（总是失败）。
===================	=======================================================

示例
----

对 ``/dev/nvme0n1`` 扇区 0 的读取，每 10 次中有一次返回
``BLK_STS_IOERR``：

	$ echo 'add,op=READ,start=0,status=IOERR,chance=10' > /sys/kernel/debug/block/nvme0n1/error_injection

对 ``/dev/nvme0n1`` 的每次写入都返回 ``BLK_STS_MEDIUM``：

	$ echo 'add,op=WRITE,start=0,status=MEDIUM' > /sys/kernel/debug/block/nvme0n1/error_injection

删除 ``/dev/nvme0n1`` 的所有规则：

	$ echo 'removeall' > /sys/kernel/debug/block/nvme0n1/error_injection
