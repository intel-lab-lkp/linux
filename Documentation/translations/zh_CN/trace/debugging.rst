.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst
:Original: Documentation/trace/debugging.rst
:翻译:

 岑发亮 Frank Cen <focksor@gmail.com>

==================
使用追踪器进行调试
==================

Copyright 2024 Google LLC.

:Author:   Steven Rostedt <rostedt@goodmis.org>
:License:  The GNU Free Documentation License, Version 1.2
          (dual licensed under the GPL v2)

- Written for: 6.12

引言
---
跟踪框架对于调试 Linux 内核非常有用。本文记录了使用追踪器进行调试的各种方法。

首先，确保已经挂载了 tracefs 文件系统::

 $ sudo mount -t tracefs tracefs /sys/kernel/tracing


使用 trace_printk()
-------------------

trace_printk() 是一个非常轻量级的工具，可以在内核中除 "noinstr" 部分外的任何上下文使用。
它可以在正常、软中断、硬中断甚至 NMI 上下文中使用。
跟踪数据以无锁的方式写入到环形缓冲区 (tracing ring buffer) 中。
为了使其更轻量，当可能时，它会只记录格式字符串的指针，并将原始参数保存到缓冲区中。格式和参数
将在读取环形缓冲区时再进行处理。这样，格式化处理就不会在热路径（即记录追踪的地方）中完成。

trace_printk() 只用于调试，绝不应添加到内核的子系统中。如果需要调试跟踪，请改用添加跟踪事件。
如果在内核中发现 trace_printk()，则 dmesg 中会出现以下内容::

  **********************************************************
  **   NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE   **
  **                                                      **
  ** trace_printk() being used. Allocating extra memory.  **
  **                                                      **
  ** This means that this is a DEBUG kernel and it is     **
  ** unsafe for production use.                           **
  **                                                      **
  ** If you see this message and you are not debugging    **
  ** the kernel, report this immediately to your vendor!  **
  **                                                      **
  **   NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE   **
  **********************************************************


调试内核崩溃
------------
有多种方法可以在内核崩溃时获取系统状态。这可以通过 printk 中的 oops 消息
来实现，也可以使用 kexec/kdump。但这些方法只能显示崩溃时的情况，而知道
在崩溃发生之前发生了什么则非常有帮助。tracing ring buffer 默认情况下是一个循环缓冲区，
它会用较新的事件覆盖较旧的事件。当崩溃发生时，ring buffer 会包含导致崩溃的所有事件。

在调试时，可以通过一些内核命令行参数来辅助。第一个参数是 ftrace_dump_on_oops，
它会在系统发生 oops 时，把 tracing ring buffer 的内容输出到控制台。如果控制台的输出被记录下来，这会非常有帮助。
但如果使用的是串口控制台，建议将环形缓冲区设得相对较小，否则转储环形缓冲区可能需要几分钟甚至数小时才能完成。
下面是一个内核命令行示例::

  ftrace_dump_on_oops trace_buf_size=50K

注意，tracing buffer 由每个 CPU 的缓冲区组成，每个缓冲区又被划分为默认大小为 PAGE_SIZE 的子缓冲区。
上面的 trace_buf_size 选项将每个 CPU 的缓冲区设置为 50K，因此在一个有 8 个 CPU 的机器上，
总共实际上是 400K。

跨重启的持久缓冲区
------------------
如果系统内存允许，可以在内存中的特定位置指定追踪环形缓冲区。如果该位置在重启时保持不变
且内存未被修改，则可以在下一次启动后读取跟踪缓冲区。有两种方法可以为环形缓冲区保留内存。

更可靠的方法（在 x86 上）是使用 memmap 内核命令行选项来保留内存，然后将该内存用于 trace_instance。
这需要对系统的物理内存布局有一定的了解。使用这种方法的优点是，环形缓冲区的内存位置将始终保持不变::

  memmap==12M$0x284500000 trace_instance=boot_map@0x284500000:12M

上面的 memmap 选项在物理内存地址 0x284500000 处保留了 12 兆字节的内存。
然后，trace_instance 选项将在同一位置创建一个名为 "boot_map" 的 trace instance，
这个实例使用与保留的内存相同的大小。由于环形缓冲区被划分为每个 CPU 的缓冲区，
因此这 12 兆字节的内存将被均匀地分配给这些 CPU。如果你有 8 个 CPU，
那么每个 CPU 的环形缓冲区大小将是 1.5 兆字节。注意，这其中还要包括元数据，
因此环形缓冲区实际使用的内存会稍微小一些。

另一种更通用但没那么可靠的在启动时分配环形缓冲区映射的方法是使用 reserve_mem 选项::

  reserve_mem=12M:4096:trace trace_instance=boot_map@trace

上面的 reserve_mem 会在启动时找到 12 兆字节的可用内存，并按 4096 字节对齐。
这块内存会被标记为 "trace" 以供后续的命令行选项使用。

trace_instance 选项创建了一个名为 "boot_map" 的 trace instance，并将使用
由 reserve_mem 保留的、标记为 "trace" 的内存。这种方法更通用，但可能不那么可靠。
由于 KASLR（内核地址空间布局随机化）的存在，reserve_mem 保留的内存位置可能会有所不同。
当这种情况发生时，环形缓冲区将不是来自上一次启动的内容，并且会被重置。

有时我们可以使用更大的对齐方式，来防止 KASLR 以某种方式移动内存位置从而改变 reserve_mem 的位置。
通过使用更大的对齐方式，可能会使缓冲区的位置更一致::

  reserve_mem=12M:0x2000000:trace trace_instance=boot_map@trace

在启动时，会对为 ring buffer 保留的内存进行验证。它会经过一系列测试，以确保 ring buffer 包含有效数据。
如果测试通过，该环形缓冲区将被设置为可从实例中读取。如果测试未通过，则会被清空并重新初始化。

这块映射内存的布局在不同的内核版本之间可能不一致，因此只有相同的内核版本才能保证工作正常。
切换到不同的内核版本可能会发现布局不同，缓冲区将会被标记为无效。

注意：映射的地址和大小都必须符合架构的页面对齐要求。

在启动阶段使用 trace_printk()
-----------------------------
默认情况下， trace_printk() 的内容会进入 top level tracing instance。
但这个 instance 在重启时不会被保留。为了让 trace_printk() 的内容
和其他一些内部 tracing（如 dump stacks）进入被保留的缓冲区，
可以从内核命令行设置 instance 为 trace_printk() 的目标，
或者在启动后通过 trace_printk_dest 选项进行设置。

启动后::

  echo 1 > /sys/kernel/tracing/instances/boot_map/options/trace_printk_dest

在内核命令行设置::

  reserve_mem=12M:4096:trace trace_instance=boot_map^traceprintk^traceoff@trace

如果在内核命令行中设置，建议同时使用 "traceoff" 标志来禁用追踪，并在启动后再启用追踪。
否则，最近一次启动的追踪信息将与上一次启动的追踪信息混在一起，可能会变得难以阅读。
