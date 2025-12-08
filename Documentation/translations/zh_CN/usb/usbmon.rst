.. SPDX-License-Identifier: GPL-2.0
.. include:: ../disclaimer-zh_CN.rst

:Original: Documentation/usb/usbmon.rst
:翻译:

 白钶凡 Kefan Bai <baikefan@leap-io-kernel.com>

:校译:



======
usbmon
======

简介
====
小写形式的"usbmon"指的是内核中的一个功能，用于收集USB总线上的I/O追踪信息。
这个功能类似于网络监控工具tcpdump(1)或Ethereal所使用的数据包套接字。
同样地，我们会使用像usbdump或USBMon（大写形式）这样的工具来检查usbmon
生成的原始追踪数据。

usbmon报告的是各个外设驱动程序向主机控制器驱动程序（HCD）发出的请求。
因此，如果HCD本身有bug，那么usbmon报告的追踪信息可能无法精确对应实际的
总线事务。这种情形与tcpdump是一样的。

目前实现了两种API：文本和二进制。二进制API通过/dev命名空间中的字符设备提供，
并且是一个ABI。文本API自内核2.6.35起废弃，但为了兼容仍然可用。

如何使用usbmon收集原始文本追踪信息
==================================

与数据包套接字不同，usbmon提供了一种接口，可以输出文本格式的追踪信息。
这样做有两个目的：
第一，在更完善的格式最终确定之前，它作为工具间通用的追踪交换格式；
第二，在不使用工具的情况下，人们也可以直接阅读这些信息。

要收集原始文本追踪信息，请按以下步骤进行操作。

1. 准备
--------

首先必须在你的内核配置中启用debugfs，之后挂载debugfs。
如果usbmon是作为模块构建的，那么还要加载usbmon模块。
如果usbmon已经编入内核，则无需加载usbmon模块。

命令示例::

    # mount -t debugfs none_debugs /sys/kernel/debug
    # modprobe usbmon
    #

确认总线socket是否存在::
    # ls /sys/kernel/debug/usb/usbmon
    0s  0u  1s  1t  1u  2s  2t  2u  3s  3t  3u  4s  4t  4u
    #

现在，你可以选择使用'0u'用来捕获所有总线上的数据包，并跳到第#3步，
或者使用第#2步找到你的设备所在的总线。
这样可以过滤掉那些持续输出数据的烦人设备。



2. 查找目标设备连接的是哪条总线
-------------------------------

运行"cat /sys/kernel/debug/usb/devices"，找到对应设备的T行。
通常你可以通过vendor字符串来查找。如果你有许多类似设备，
可以拔掉其中一个设备，并比较前后两次
/sys/kernel/debug/usb/devices的输出。
T行会包含总线编号。

示例::

  T:  Bus=03 Lev=01 Prnt=01 Port=00 Cnt=01 Dev#=  2 Spd=12  MxCh= 0
  D:  Ver= 1.10 Cls=00(>ifc ) Sub=00 Prot=00 MxPS= 8 #Cfgs=  1
  P:  Vendor=0557 ProdID=2004 Rev= 1.00
  S:  Manufacturer=ATEN
  S:  Product=UC100KM V2.00

"Bus=03"表示设备在总线3上。或者，你可以查看lsusb的输出，并从对应行得到总线编号。

示例：
Bus 003 Device 002: ID 0557:2004 ATEN UC100KM V2.00


3. 启动 'cat'

::

    # cat /sys/kernel/debug/usb/usbmon/3u > /tmp/1.mon.out

用于监听单条总线。

如果要监听所有总线，则输入::

    # cat /sys/kernel/debug/usb/usbmon/0u > /tmp/1.mon.out

此进程会一直读取，直到被终止。
因为输出通常会非常长，所以更推荐将输出重定向到某个位置。


4. 在USB总线上执行期望的操作
----------------------------

此处你需要执行一些会产生USB流量的动作，比如：插入U盘、拷贝文件、操作摄像头等。


5. 停止cat
-----------

这一步通常需要通过键盘中断（Control-C）来完成。

此时输出文件（本例中为 /tmp/1.mon.out）可以保存、通过电子邮件发送，
或使用文本编辑器查看。
如果使用最后一种方式，请确保文件不会大到编辑器无法打开。


原始文本数据格式
================

目前支持两种格式：原始格式（即1t格式）和1u格式。
1t格式在内核2.6.21中就已经被废弃。
1u格式增加了一些新字段，如ISO帧描述符、interval等。
它生成的行稍微长一些，但在格式上是1t的严格超集。

如果程序需要区分上述两种格式，可以查看address字段（见下）。
若包含两个冒号，则为1t格式，否则为1u。

任何文本格式的数据由一系列事件组成，如URB提交、URB回调、提交错误等。
每个事件对应单独的一行文本，由使用空白符间隔的若干字段组成。
字段的数量与位置可能取决于事件类型，但以下字段对所有类型都通用：

共有字段按照从左到右的顺序依次列出：

- URB Tag。用于标识URB，通常是URB结构体在内核中的地址（以十六进制表示），
  但也可能是序号或其他合理的唯一字符串。

- 时间戳（微秒）。十进制数字。精度取决于可用时钟，因此可能比1微秒更差。

- 事件类型。表示事件的格式，而不是URB的类型。
  有效类型包括：S - 提交，C - 回调，E - 提交错误。

- Address字段（以前称为pipe）。包含四个冒号分隔的字段：
  URB类型与方向，总线号，设备地址，端点号。
  类型与方向的编码如下：

    == ==   =====================
    Ci Co   控制input/output
    Zi Zo   等时input/output
    Ii Io   中断input/output
    Bi Bo   批量input/output
    == ==   =====================

  总线号、设备地址和端点号使用十进制，但可能有前导零。

- URB状态字。这个字段可以是一个字母，也可以是由冒号分隔的几个数字：
  URB状态、间隔（interval）、起始帧（start frame）以及错误计数（error count）。
  与地址字段不同，除了状态之外，其余所有字段都是可选的。
  interval仅在中断和等时URB中打印；
  起始帧仅在等时URB中打印；
  错误数量仅在等时回调事件中打印。

  状态字段是一个十进制数字，有时为负数，对应URB的状态字段。
  对于提交（submission），这个字段没有实际意义，
  但是为了便于脚本解析，所以依然存在。
  当发生错误时，该字段包含错误码。

  在提交控制数据包的情况下，该字段包含一个Setup Tag，而不是一组数字。
  判断是否存在Setup Tag很容易，因为它从来不是数字。
  因此，如果脚本在该字段中发现一组数字，
  它们会继续读取数据长度（等时URB除外）。
  如果发现的是其他内容，比如一个字母，
  那么脚本会在读取数据长度或等时描述符之前，会先读取Setup包。

- Setup包由5个字段构成：bmRequestType, bRequest, wValue, wIndex, wLength。
  符合USB 2.0 规范。如果Setup标签为's'，这些字段是安全可解码的。
  否则，Setup数据包虽然存在，但未被捕获，其字段中会填充占位内容。

- 等时传输帧描述符数量及其内容：
  如果一次同步传输事件包含一组描述符，首先打印URB中描述符的总数，
  然后依次打印每个描述符的一个字段，最多打印5个字段。
  每个字段由三个用冒号分隔的十进制数字组成，
  分别表示状态（status）、偏移（offset）和长度（length）。
  对于提交（submission），打印的是初始长度；
  对于回调（callback），打印的是实际长度。

- 数据长度：
  对于提交，表示请求的长度；对于回调，表示实际传输的长度。

- 数据标签：
  即使数据长度非零，usbmon也可能不会捕获数据。仅当标签为'='时，才会有数据字段存在。

- 数据字段：
  按大端十六进制格式显示。
  注意，这些并非真正的机器字，而只是将字节流拆分为若干字，
  以便阅读。因此最后一个字可能包含1到4个字节。
  收集的数据长度是有限的，可能小于数据长度字段中报告的值。
  因为数据长度字段只统计实际接收到的字节，而数据字段包含整个传输缓冲区，
  所以，在等时输入（Zi）完成且缓冲区中接收到的数据稀疏的情况下，
  收集的数据长度可能大于数据长度字段的值。



示例：

获取端口状态的输入控制传输::

    d5ea89a0 3575914555 S Ci:1:001:0 s a3 00 0000 0003 0004 4 <
    d5ea89a0 3575914560 C Ci:1:001:0 0 4 = 01050000

向地址为5的存储设备发送31字节Bulk封装的SCSI命令0x28（READ_10）的输出批量传输::

    dd65f0e8 4128379752 S Bo:1:005:2 -115 31 = 55534243 ad000000 00800000 80010a28 20000000 20000040 00000000 000000
    dd65f0e8 4128379808 C Bo:1:005:2 0 31 >

原始二进制格式与API
===================
API的整体架构与上面提到的基本相同，
只是事件以二进制格式传输。
每个事件使用以下结构发送（名称为自定义，方便引用）::


  struct usbmon_packet {
	u64 id;			/*  0: URB ID - 从提交到回调 */
	unsigned char type;	/*  8: 与文本相同；可扩展 */
	unsigned char xfer_type; /*    ISO (0), 中断, 控制, 批量 (3) */
	unsigned char epnum;	/*     端点号和传输方向 */
	unsigned char devnum;	/*     设备地址 */
	u16 busnum;		/* 12: 总线号 */
	char flag_setup;	/* 14: 与文本相同 */
	char flag_data;		/* 15: 与文本相同；二进制零也可 */
	s64 ts_sec;		/* 16: gettimeofday */
	s32 ts_usec;		/* 24: gettimeofday */
	int status;		/* 28: */
	unsigned int length;	/* 32: 数据长度（提交或实际） */
	unsigned int len_cap;	/* 36: 实际传输长度 */
	union {			/* 40: */
		unsigned char setup[SETUP_LEN];	/* 仅用于控制 S类型 */
		struct iso_rec {		/* ISO专用 */
			int error_count;
			int numdesc;
		} iso;
	} s;
	int interval;		/* 48: 仅用于中断和ISO */
	int start_frame;	/* 52: ISO专用 */
	unsigned int xfer_flags; /* 56: URB的transfer_flags副本 */
	unsigned int ndesc;	/* 60: 实际ISO描述符数量 */
  };				/* 64 总长度 */

可以使用read(2)，ioctl(2)或通过mmap访问缓冲区，从字符设备读取这些事件。
然而，出于兼容性原因，使用read(2)只返回前48字节。

字符设备通常命名为/dev/usbmonN，其中N是USB总线号。
编号为零的设备 (/dev/usbmon0) 表示所有总线。
请注意，具体命名策略由Linux发行版设定。

如果手动创建/dev/usbmon0，确保它归root所有，并且权限为0600。
否则，非特权用户将能够监听键盘数据。

以下MON_IOC_MAGIC为0x92的ioctl可用：

  MON_IOCQ_URB_LEN，定义为_IO(MON_IOC_MAGIC, 1)

该调用返回下一个事件的数据长度。注意大多数事件不包含数据，
因此如果该调用返回零，并不意味着没有事件。

  MON_IOCG_STATS，定义为_IOR(MON_IOC_MAGIC, 3, struct mon_bin_stats)

参数是指向以下结构的指针::

  struct mon_bin_stats {
	u32 queued;
	u32 dropped;
  };

成员queued表示当前缓冲区中排队的事件数量（不是自上次重置以来处理的事件数量）。

成员dropped表示自上次调用MON_IOCG_STATS以来丢失的事件数量。

  MON_IOCT_RING_SIZE，定义为_IO(MON_IOC_MAGIC, 4)

此调用设置缓冲区大小。参数为以字节为单位的缓冲区大小。
大小可能会向下取整到下一个块（或页）。
如果请求的大小超出该内核的 [未指定] 范围，则调用失败并返回-EINVAL。



MON_IOCQ_RING_SIZE，定义为_IO(MON_IOC_MAGIC, 5)

该调用返回缓冲区当前大小（以字节为单位）。

MON_IOCX_GET，定义为_IOW(MON_IOC_MAGIC, 6, struct mon_get_arg)
MON_IOCX_GETX，定义为_IOW(MON_IOC_MAGIC, 10, struct mon_get_arg)

如果内核缓冲区中没有事件时，这些调用会一直等待事件到达，然后返回第一个事件。
参数是指向以下结构的指针::

  struct mon_get_arg {
	struct usbmon_packet *hdr;
	void *data;
	size_t alloc;		/* 数据长度可以为零 */
  };


调用前，应填写hdr、data和alloc字段。
调用返回后，hdr指向的区域包含下一个事件的结构。
如果有数据，那么数据缓冲区包含数据。
该次调用会从内核缓冲区中移除该事件。

MON_IOCX_GET将48字节的数据复制到hdr区域，MON_IOCX_GETX会复制64字节。

MON_IOCX_MFETCH，定义为_IOWR(MON_IOC_MAGIC, 7, struct mon_mfetch_arg)

当应用程序通过mmap(2)访问缓冲区时，主要使用该ioctl。
其参数是指向以下结构的指针::

  struct mon_mfetch_arg {
	uint32_t *offvec;	/* 获取的事件偏移向量 */
	uint32_t nfetch;	/* 要获取的事件数量（输出：已获取） */
	uint32_t nflush;	/* 要刷新事件数量 */
  };


ioctl操作分为三个阶段：

首先，从内核缓冲区移除并丢弃最多nflush个事件。
实际丢弃的事件数量存储在nflush中。

其次，除非伪设备以O_NONBLOCK打开，否则会一直等待，直到缓冲区中有事件出现。

第三，将最多nfetch个偏移量提取到mmap缓冲区，并存储
到offvec中。实际的事件偏移数量存储在nfetch中。

 MON_IOCH_MFLUSH，定义为_IO(MON_IOC_MAGIC, 8)

此调用从内核缓冲区移除若干事件。其参数为要移除的事件数量。
如果缓冲区中的事件少于请求数量，则移除所有事件，且不报告错误。
当没有事件时也可使用。

 FIONBIO

如有需要，将来可能实现ioctl FIONBIO。

除了ioctl(2)和read(2)，可以使用
select(2)和poll(2)轮询二进制API的特殊文件。
但lseek(2)无效。

* 二进制API的内核缓冲区内存映射访问

基本思想很简单：

准备阶段，获取当前大小并使用mmap(2)映射缓冲区。
然后执行类似下面伪代码的循环::

   struct mon_mfetch_arg fetch;
   struct usbmon_packet *hdr;
   int nflush = 0;
   for (;;) {
      fetch.offvec = vec; // 有N个32位字
      fetch.nfetch = N;   // 或少于N
      fetch.nflush = nflush;
      ioctl(fd, MON_IOCX_MFETCH, &fetch);   // 同时处理错误
      nflush = fetch.nfetch;       // 完成后要刷新这么多包'/
      for (i = 0; i < nflush; i++) {
         hdr = (struct ubsmon_packet *) &mmap_area[vec[i]];
         if (hdr->type == '@')     // 填充包
            continue;
         caddr_t data = &mmap_area[vec[i]] + 64;
         process_packet(hdr, data);
      }
   }



因此，主要思想是每N个事件只执行一次ioctl。

虽然缓冲区是环形的，但返回的头和数据不会跨越缓冲区末端，
因此上面的伪代码无需任何合并操作。
