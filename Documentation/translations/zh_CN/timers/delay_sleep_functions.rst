.. SPDX-License-Identifier: GPL-2.0

.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/timers/delay_sleep_functions.rst

:翻译:

  徐兴球 Xingqiu Xu <hilbertanjou83@gmail.com>

==============
延迟和睡眠机制
==============

本文档旨在回答一个常见问题："插入延迟的正
确方法(TM)是什么？"

驱动程序开发者最常面对这个问题，他们必须处
理硬件延迟，但可能对Linux内核的内部工作机
制不是特别熟悉。

下表粗略概述了现有函数"系列"及其局限性。
此概述表格不能替代使用前阅读函数描述！

.. list-table::
   :widths: 20 20 20 20 20
   :header-rows: 2

   * -
     - `*delay()`
     - `usleep_range*()`
     - `*sleep()`
     - `fsleep()`
   * -
     - 忙等待循环
     - 基于 hrtimers
     - 基于 timer list timers
     - 结合其他方法
   * - 原子上下文中的使用
     - 是
     - 否
     - 否
     - 否
   * - "短间隔"上精确
     - 是
     - 是
     - 视情况而定
     - 是
   * - "长间隔"上精确
     - 不要使用！
     - 是
     - 最大 12.5% 误差
     - 是
   * - 可中断变体
     - 否
     - 是
     - 是
     - 否

对于非原子上下文的通用建议可能是：

#. 当不确定时使用 `fsleep()` （因为它结合
   了其他方法的所有优点）
#. 尽可能使用 `*sleep()`
#. 当 `*sleep()` 的精度不够时使用
   `usleep_range*()`
#. 对于非常非常短的延迟使用 `*delay()`

在接下来的章节中可以找到有关函数"系列"的更
详细信息。

`*delay()` 函数系列
-------------------

这些函数使用基于时钟速度的 jiffy 估算，并
忙等待足够的循环周期以实现所需的延迟。
udelay() 是基本实现，ndelay() 和 mdelay()
是变体。

这些函数主要用于在原子上下文中添加延迟。请
确保在原子上下文中添加延迟之前问自己：这真
的需要吗？

.. kernel-doc:: include/asm-generic/delay.h
	:identifiers: udelay ndelay

.. kernel-doc:: include/linux/delay.h
	:identifiers: mdelay


`usleep_range*()` 和 `*sleep()` 函数系列
-----------------------------------------

这些函数使用 hrtimers 或 timer list 定
时器来提供所请求的睡眠持续时间。为了决定使
用哪个函数是正确的，请考虑一些基本信息：

#. hrtimers 更昂贵，因为它们使用红黑树
   （而不是散列表）
#. 当请求的睡眠时间是最早的定时器时，
   hrtimers 更昂贵，这意味着必须对真实硬
   件进行编程
#. timer list 定时器总会存在一定误差，
   因为它们基于 jiffy

通用建议在此重复：

#. 当不确定时使用 `fsleep()` （因为它结合
   了其他方法的所有优点）
#. 尽可能使用 `*sleep()`
#. 当 `*sleep()` 的精度不够时使用
   `usleep_range*()`

首先检查 fsleep() 函数描述，要了解更多关于
精度的信息，请检查 msleep() 函数描述。


`usleep_range*()`
~~~~~~~~~~~~~~~~~

.. kernel-doc:: include/linux/delay.h
	:identifiers: usleep_range usleep_range_idle

.. kernel-doc:: kernel/time/sleep_timeout.c
	:identifiers: usleep_range_state


`*sleep()`
~~~~~~~~~~

.. kernel-doc:: kernel/time/sleep_timeout.c
       :identifiers: msleep msleep_interruptible

.. kernel-doc:: include/linux/delay.h
	:identifiers: ssleep fsleep
