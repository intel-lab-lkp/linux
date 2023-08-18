.. SPDX-License-Identifier: GPL-2.0

:Original: Documentation/mm/memory-model.rst

:翻译:

 司延腾 Yanteng Si <siyanteng@loongson.cn>

:校译:


============
物理内存模型
============

系统中的物理内存可以用不同的方式进行寻址。最简单的情况是，物理内存从地址 0 开
始，跨越一个连续的范围，直到最大的物理内存地址。然而，这个地址范围中可能包含
CPU 无法访问的小孔隙。那么，物理内存的地址范围可能呈现为几个不同的连续地址范
围。而且，还有 NUMA，即不同的内存器件连接到不同的 CPU 这种情况。

Linux 使用两种内存模型中的一种对这种多样性进行抽象：FLATMEM 和 SPARSEMEM。
每个 Linux 支持的架构都定义了架构所支持的内存模型；默认的内存模型，以及是否
有可能手动覆盖该默认值。

所有的内存模型都使用排列在一个或多个数组中的 `struct page` 来跟踪物理页框的
状态。

无论选择哪种内存模型，物理页框号（PFN）和相应的 `struct page` 之间都存
在一对一的映射关系。

每个内存模型都定义了 :c:func:`pfn_to_page` 和 :c:func:`page_to_pfn`
帮助函数，实现 PFN 和 `struct page` 之间的双向转换。

FLATMEM
=======

最简单的内存模型是 FLATMEM。这个模型适用于非 NUMA 系统的连续或大部分连续的
物理内存。

在 FLATMEM 内存模型中，有一个全局的 `mem_map` 数组来映射整个物理内存。对
于大多数架构，如果存在‘内存空洞’，则‘空洞’在 `mem_map` 数组中都有条目。与
空洞相对应的 `struct page` 对象不会被完全初始化。

为了分配 `mem_map` 数组，架构特定的初始化代码(mem_init)应该调用
free_area_init() 函数。并且，在 memblock_free_all() 函数被调用之前，mem_map
数组是不能使用的。在 memblock_free_all 函数中，将所有的内存交给伙伴分配器。

特定架构的实现可能会释放 `mem_map` 数组中不包括实际物理页的部分。此时，架构
特定的 :c:func:`pfn_valid` 实现应该考虑到识别 `mem_map` 中存在的‘空洞’并恰当
的处理之。

使用 FLATMEM 模型，PFN 和 `struct page` 之间的转换很简便：
`PFN - ARCH_PFN_OFFSET` 可以直接作为 `mem_map` 数组的索引使用。
架构特定的定义：
`ARCH_PFN_OFFSET` 定义了物理内存起始地址对应的第一个页框号。当然如果物理内存
的起始地址是 0 那么这个宏可能不会定义。

SPARSEMEM
=========

SPARSEMEM 是 Linux 内核中最通用的内存模型，若干高级功能如物理内存的热插拔、非
易失性内存设备的替代内存图以及大系统的内存图延迟初始化等都只基于 SPARSEMEM 内
存模型实现。

SPARSEMEM 模型将物理内存显式分为内存区段的集合。内存区段用 mem_section 结构
体表示，它包含 `section_mem_map` ，从逻辑上讲，它是一个指向 `struct page`
阵列的指针。然而，在这个 section_mem_map 中还编码了其他用以帮助分区管理的数值。
区段的大小和最大区段数使用 `SECTION_SIZE_BITS` 和 `MAX_PHYSMEM_BITS` 常量
来指定，这两个常量由每个支持 SPARSEMEM 的架构定义。 `MAX_PHYSMEM_BITS`
是特定架构所支持的物理地址的实际宽度，而 `SECTION_SIZE_BITS` 是交由特定架构自由
发挥。

最大的段数表示为 `NR_MEM_SECTIONS` ，定义为:

.. math::

   NR\_MEM\_SECTIONS = 2 ^ {(MAX\_PHYSMEM\_BITS - SECTION\_SIZE\_BITS)}

`mem_section` 对象被安排在一个叫做 `mem_sections` 的二维数组中。这个数组的
大小和位置取决于 `CONFIG_SPARSEM_EXTREME` 和可能的最大段数:

* 当 `CONFIG_SPARSEMEM_EXTREME` 被禁用时， `mem_sections` 数组是静态的，有
  `NR_MEM_SECTIONS` 行。每一行持有一个 `mem_section` 对象。
* 当 `CONFIG_SPARSEMEM_EXTREME` 被启用时， `mem_sections` 数组被动态分配。
  每一行包含价值 `PAGE_SIZE` 的 `mem_section` 对象，行数的计算是为了适应所有的
  内存区。

架构特定的设置代码需要调用 sparse_init() 来初始化内存区和内存映射。

基于 SPARSEMEM 模型，有两种可能的方式将 PFN 转换为相应的 `struct page` --
"classic sparse" 和 "sparse vmemmap"。具体选择哪一种由内核配置选项
`CONFIG_SPARSEMEM_VMEMMAP` 的值决定。在内核构建之前需要选择是哪一种。

Classic sparse 在 page->flags 中编码了一个页面的段号，并使用 PFN 的高位来访问
映射该页框的区段。在一个区段内，PFN 是指向页数组的索引。

Sparse vmemmap 使用虚拟映射的内存映射来优化 pfn_to_page 和 page_to_pfn 操
作。有一个全局的 `struct page *vmemmap` 指针，指向一个虚拟连续的 `struct page`
数组对象。PFN 是该数组的索引，`struct page` 在 `vmemmap` 中的偏移量就是该页的
PFN。

为了使用 vmemmap，特定架构必须保留一段虚拟地址围，以映射包含内存映射图的物理页，
并确保 `vmemmap` 指向该范围。此外，架构特定代码需要实现
:c:func:`vmemmap_populate` 方法，在这个方法中，需要分配物理内存并为虚拟内存映
射图创建页表。如果特定架构对 vmemmap 映射没有任何特殊要求，它可以使用通用内存
管理提供的默认实现 :c:func:`vmemmap_populate_basepages`。

虚拟映射的内存映射图允许将持久性内存设备的 `struct page` 对象存储在这些设备上
预先分配好的（内存）存储中。这种存储用 vmem_altmap 数据结构表示，最终通过一长
串的函数调用传递给 vmemmap_populate() 函数。vmemmap_populate() 的实现里面可
使用 `vmem_altmap` 和 :c:func:`vmemmap_alloc_block_buf` 助手函数来从持久性内
存设备上为内存映射图分配内存。

ZONE_DEVICE
===========
`ZONE_DEVICE` 设施建立在 `SPARSEM_VMEMMAP` 之上，为设备驱动识别的物理地址范
围提供 `struct page` 和 `mem_map` 服务。 `ZONE_DEVICE` 的 "设备" 方面与以下
事实有关：这些地址范围的页面对象不会被标记为在线；并且必须增加对设备的引用计
数，而不仅仅是页面的引用计数，以保持相关数据被“锁定”在内存中。
`ZONE_DEVICE` ，通过 :c:func:`devm_memremap_pages` ，为给定 pfns 范围内的内
存提供足够的内存热插拔支持来开启 :c:func:`pfn_to_page`，
:c:func:`page_to_pfn`, 和 :c:func:`get_user_pages` 服务。由于页面引
用计数永远不会低于 1，所以页面永远不会被标记为空闲内存，页面的
`struct list_head lru` 空间被重新利用，用于向映射该内存的主机设备/驱动程序进
行反向引用。

虽然 `SPARSEMEM` 将全部内存抽象为区段的集合，可以选择收集并合成内存块，但
`ZONE_DEVICE` 用户需要更小的颗粒度来用作 `mem_map` 。鉴于 `ZONE_DEVICE`
内存从未被标记为在线，因此它的内存范围不会通过 sysfs 内存热插拔 api 暴露在
内存块边界上。这个实现依赖于这种缺乏用户接口的约束，允许小于段大小的内存范围被
指定给 :c:func:`arch_add_memory` ，即内存热插拔的上半部分。子段支持 2MB 作为
:c:func:`devm_memremap_pages` 的跨架构通用对齐颗粒度。

`ZONE_DEVICE` 的用户有:

* pmem: 通过 DAX 映射使用平台持久性内存作为直接 I/O 的目标。

* hmm: 用 `->page_fault()` 和 `->page_free()` 事件回调扩展 `ZONE_DEVICE` ，
  以允许设备驱动程序协调与设备内存相关的内存管理事件，典型的设备内存有 GPU 内
  存等。详情可参考 Documentation/mm/hmm.rst。

* p2pdma: 创建 `struct page` 对象，允许 PCI/-E 设备拓扑结构中的 peer 设备协
  调它们之间的直接 DMA 操作，比如：绕过主机内存在 peer 之间直接 DMA。
