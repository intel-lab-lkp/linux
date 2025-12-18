.. SPDX-License-Identifier: GPL-2.0

.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/timers/hpet.rst

:翻译:

  徐兴球 Xingqiu Xu <hilbertanjou83@gmail.com>

================================
Linux 高精度事件定时器驱动程序
================================

高精度事件定时器（HPET）硬件遵循 Intel 和
Microsoft 的规范，修订版 1。

每个 HPET 有一个固定速率计数器（以 10 MHz
以上运行，因此称为"高精度"）和最多 32 个比
较器。通常提供三个或更多比较器，每个比较器
都可以生成单次中断，并且至少其中一个具有支
持周期中断的额外硬件。比较器也称为"定时器"，
这可能引起混淆，因为通常定时器彼此独立……这
些共享一个计数器，使重置变得复杂。

HPET 设备可以支持两种中断路由模式。在一种
模式下，比较器是没有特定系统角色的附加中断
源。许多 x86 BIOS 编写者根本不路由 HPET
中断，这导致该模式无法使用。它们支持另一种
"传统替换"模式，其中前两个比较器替代 8254
定时器和 RTC 的中断。

该驱动程序支持在驱动程序 module_init 例程
被调用之前检测 HPET 驱动分配并初始化
HPET。这使得使用定时器 0 或 1 作为主定时器
的平台代码能够拦截 HPET 初始化。可以在
arch/x86/kernel/hpet.c 中找到此初始化的示
例。

该驱动程序提供了一个用户空间 API，类似于
RTC 驱动程序框架中的 API。示例用户空间程
序在 file:samples/timers/hpet_example.c
中提供。

