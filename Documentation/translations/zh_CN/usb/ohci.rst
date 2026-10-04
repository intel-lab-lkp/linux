.. SPDX-License-Identifier: GPL-2.0

:Original: Documentation/usb/ohci.rst

:翻译: 欧阳家铭 Jiaming Ouyang <jiamingouyang13@gmail.com>

========
OHCI驱动
========

2002年8月23日

“OHCI-HCD”驱动程序是一个USB主机控制器程序（HCD），它源自2.4内核系列中的“usb-ohci”
驱动程序。“usb-ohci”代码主要由 Roman Weissgaerber <weissg@vienna.at>编写但也获
得了来自David Brownell <dbrownell@users.sourceforge.net>，Linus Torvalds
和Gregory P. Smith的支持。

它支持“开放主机控制器接口”（OHCI），该接口标准化了用于与USB 1.1主机控制器通信的
硬件寄存器协议。“与英特尔早期的“通用主机控制器接口”（UHCI）相比，它将更多只能功能
注入到硬件中。除英特尔和威盛之外的其他供应商的USB1.1控制器通常使用OHCI。

自2.4内核以来的更改包括

         - 提高了健壮性; 修复了错误; 减少了开销
         - 支持更新和简化的usbcore API
         - 中断传输可以更大，并且可以排队
         - 通过使用上层“hcd”框架，减少代码量
         - 支持一些非PCI的OHCI实现
         - ... 以及更多

“OHCI-hcd”驱动程序处理所有USB 1.1传输类型。所有类型的传输都可以排队。在“usb-ohci”
中也是如此，但中断传输除外。以前，使用一帧的周期会因为IRQ处理的开销而导致数据丢
失的风险。当中断传输排队时，可以通过确保硬件在操作系统处理相关IRQ时始终有传输需要
处理，来最大限度的降低这些风险。

- David Brownell
  <dbrownell@users.sourceforge.net>

