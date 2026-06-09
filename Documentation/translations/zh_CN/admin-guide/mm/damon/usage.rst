.. SPDX-License-Identifier: GPL-2.0
.. include:: ../../../disclaimer-zh_CN.rst

:Original: Documentation/admin-guide/mm/damon/usage.rst

:翻译:

 司延腾 Yanteng Si <siyanteng@loongson.cn>

:校译:

========
详细用法
========

DAMON 为不同的用户提供如下接口。

- *专用 DAMON 模块。*
  :ref:`这 <damon_modules_special_purpose_zh_CN>` 适用于构建、发布和/或管理带有专用 DAMON 用法内核
  的人员。使用该接口，用户可以在构建、启动或运行时，以简单的方式为给定目的使用 DAMON 的主要
  功能。
- *DAMON 用户空间工具。*
  `这 <https://github.com/damonitor/damo>`_ 适用于系统管理员等希望获得开箱即用、人性化界面的
  特权用户。使用该工具，用户可以以人性化的方式使用 DAMON 的主要功能。不过，它可能不会针对特殊
  场景进行高度调优。更多细节请参考它的 `使用文档
  <https://github.com/damonitor/damo/blob/next/USAGE.md>`_。
- *sysfs 接口。*
  :ref:`这 <sysfs_interface_zh_CN>` 适用于希望更优化地使用 DAMON 的特权用户空间程序员。使用该
  接口，用户可以通过读取和写入特殊的 sysfs 文件来使用 DAMON 的主要功能。因此，你可以编写并使用
  个性化的 DAMON sysfs 包装程序，由它代替你读写 sysfs 文件。`DAMON 用户空间工具
  <https://github.com/damonitor/damo>`_ 就是这类程序的一个例子。
- *内核空间编程接口。*
  :doc:`这 </mm/damon/api>` 适用于内核空间程序员。使用该接口，用户可以通过为自己编写内核空间
  DAMON 应用程序，以最灵活、最高效的方式利用 DAMON 的每一项功能。你甚至可以为各种地址空间扩展
  DAMON。详细信息请参考接口 :doc:`文档 </mm/damon/api>`。

.. _sysfs_interface_zh_CN:

sysfs 接口
==========

定义 ``CONFIG_DAMON_SYSFS`` 时会构建 DAMON sysfs 接口。它会在自己的 sysfs 目录
``<sysfs>/kernel/mm/damon/`` 下创建多个目录和文件。你可以通过写入和读取该目录下的文件来控制
DAMON。

作为一个简短示例，用户可以如下监测给定工作负载的虚拟地址空间。::

    # cd /sys/kernel/mm/damon/admin/
    # echo 1 > kdamonds/nr_kdamonds && echo 1 > kdamonds/0/contexts/nr_contexts
    # echo vaddr > kdamonds/0/contexts/0/operations
    # echo 1 > kdamonds/0/contexts/0/targets/nr_targets
    # echo $(pidof <workload>) > kdamonds/0/contexts/0/targets/0/pid_target
    # echo on > kdamonds/0/state

文件层次结构
------------

DAMON sysfs 接口的文件层次结构如下所示。在下图中，父子关系用缩进表示，每个目录都带有 ``/``
后缀，每个目录中的文件用逗号（","）分隔。

.. parsed-literal::

    :ref:`/sys/kernel/mm/damon <sysfs_root_zh_CN>`/admin
    │ :ref:`kdamonds <sysfs_kdamonds_zh_CN>`/nr_kdamonds
    │ │ :ref:`0 <sysfs_kdamond_zh_CN>`/state,pid,refresh_ms
    │ │ │ :ref:`contexts <sysfs_contexts_zh_CN>`/nr_contexts
    │ │ │ │ :ref:`0 <sysfs_context_zh_CN>`/avail_operations,operations,addr_unit
    │ │ │ │ │ :ref:`monitoring_attrs <sysfs_monitoring_attrs_zh_CN>`/
    │ │ │ │ │ │ intervals/sample_us,aggr_us,update_us
    │ │ │ │ │ │ │ intervals_goal/access_bp,aggrs,min_sample_us,max_sample_us
    │ │ │ │ │ │ nr_regions/min,max
    │ │ │ │ │ :ref:`targets <sysfs_targets_zh_CN>`/nr_targets
    │ │ │ │ │ │ :ref:`0 <sysfs_target_zh_CN>`/pid_target,obsolete_target
    │ │ │ │ │ │ │ :ref:`regions <sysfs_regions_zh_CN>`/nr_regions
    │ │ │ │ │ │ │ │ :ref:`0 <sysfs_region_zh_CN>`/start,end
    │ │ │ │ │ │ │ │ ...
    │ │ │ │ │ │ ...
    │ │ │ │ │ :ref:`schemes <sysfs_schemes_zh_CN>`/nr_schemes
    │ │ │ │ │ │ :ref:`0 <sysfs_scheme_zh_CN>`/action,target_nid,apply_interval_us
    │ │ │ │ │ │ │ :ref:`access_pattern <sysfs_access_pattern_zh_CN>`/
    │ │ │ │ │ │ │ │ sz/min,max
    │ │ │ │ │ │ │ │ nr_accesses/min,max
    │ │ │ │ │ │ │ │ age/min,max
    │ │ │ │ │ │ │ :ref:`quotas <sysfs_quotas_zh_CN>`/ms,bytes,reset_interval_ms,effective_bytes,goal_tuner
    │ │ │ │ │ │ │ │ weights/sz_permil,nr_accesses_permil,age_permil
    │ │ │ │ │ │ │ │ :ref:`goals <sysfs_schemes_quota_goals_zh_CN>`/nr_goals
    │ │ │ │ │ │ │ │ │ 0/target_metric,target_value,current_value,nid,path
    │ │ │ │ │ │ │ :ref:`watermarks <sysfs_watermarks_zh_CN>`/metric,interval_us,high,mid,low
    │ │ │ │ │ │ │ :ref:`{core_,ops_,}filters <sysfs_filters_zh_CN>`/nr_filters
    │ │ │ │ │ │ │ │ 0/type,matching,allow,memcg_path,addr_start,addr_end,target_idx,min,max
    │ │ │ │ │ │ │ :ref:`dests <damon_sysfs_dests_zh_CN>`/nr_dests
    │ │ │ │ │ │ │ │ 0/id,weight
    │ │ │ │ │ │ │ :ref:`stats <sysfs_schemes_stats_zh_CN>`/nr_tried,sz_tried,nr_applied,sz_applied,sz_ops_filter_passed,qt_exceeds,nr_snapshots,max_nr_snapshots
    │ │ │ │ │ │ │ :ref:`tried_regions <sysfs_schemes_tried_regions_zh_CN>`/total_bytes
    │ │ │ │ │ │ │ │ 0/start,end,nr_accesses,age,sz_filter_passed
    │ │ │ │ │ │ │ │ ...
    │ │ │ │ │ │ ...
    │ │ │ │ ...
    │ │ ...

.. _sysfs_root_zh_CN:

根
--

DAMON sysfs 接口的根是 ``<sysfs>/kernel/mm/damon/``，它有一个名为 ``admin`` 的目录。该目录
包含供特权用户空间程序控制 DAMON 的文件。具有 root 权限的用户空间工具或守护进程可以使用该目录。

.. _sysfs_kdamonds_zh_CN:

kdamonds/
---------

在 ``admin`` 目录下，存在一个名为 ``kdamonds`` 的目录，其中包含控制 kdamonds 的文件（更多细节
请参考 :doc:`设计文档 </mm/damon/design>`）。开始时，该目录只有一个文件 ``nr_kdamonds``。向该
文件写入一个数字（``N``）会创建 ``0`` 到 ``N-1`` 这些子目录。
每个目录代表一个 kdamond。

.. _sysfs_kdamond_zh_CN:

kdamonds/<N>/
-------------

每个 kdamond 目录中存在三个文件（``state``、``pid`` 和 ``refresh_ms``）以及一个目录
（``contexts``）。

读取 ``state`` 时，如果 kdamond 当前正在运行，则返回 ``on``，否则返回 ``off``。

用户可以向 ``state`` 文件写入以下命令来控制 kdamond。

- ``on``：开始运行。
- ``off``：停止运行。
- ``commit``：重新读取除 ``state`` 文件以外的 sysfs 文件中的用户输入。如果没有指定目标区域，
  监测 :ref:`目标区域 <sysfs_regions_zh_CN>` 输入也会被忽略。
- ``update_tuned_intervals``：使用自动调优后的 ``采样间隔`` 和 ``聚集间隔`` 更新该 kdamond 的
  ``sample_us`` 和 ``aggr_us`` 文件内容。更多细节请参考 :ref:`intervals_goal 小节
  <damon_usage_sysfs_monitoring_intervals_goal_zh_CN>`。
- ``commit_schemes_quota_goals``：读取基于 DAMON 的操作方案的 :ref:`配额目标
  <sysfs_schemes_quota_goals_zh_CN>`。
- ``update_schemes_stats``：更新该 kdamond 的每个基于 DAMON 的操作方案的统计文件内容。关于统计
  信息的细节，请参考 :ref:`stats 小节 <sysfs_schemes_stats_zh_CN>`。
- ``update_schemes_tried_regions``：更新该 kdamond 的每个基于 DAMON 的操作方案的动作尝试区域目
  录。关于基于 DAMON 的操作方案动作尝试区域目录的细节，请参考 :ref:`tried_regions 小节
  <sysfs_schemes_tried_regions_zh_CN>`。
- ``update_schemes_tried_bytes``：只更新 ``.../tried_regions/total_bytes`` 文件。
- ``clear_schemes_tried_regions``：清除该 kdamond 的每个基于 DAMON 的操作方案的动作尝试区域目
  录。
- ``update_schemes_effective_quotas``：更新该 kdamond 的每个基于 DAMON 的操作方案的
  ``effective_bytes`` 文件内容。更多细节请参考 :ref:`quotas 目录 <sysfs_quotas_zh_CN>`。

如果状态为 ``on``，读取 ``pid`` 会显示 kdamond 线程的 pid。

用户可以要求内核周期性地更新显示自动调优参数和 DAMOS 统计信息的文件，而不是手动向 ``state``
文件写入 ``update_tuned_intervals`` 等关键字。为此，用户应向 ``refresh_ms`` 文件写入期望的更新
时间间隔（毫秒）。如果间隔为零，则禁用周期性更新。读取该文件会显示当前设置的时间间隔。

``contexts`` 目录包含用于控制该 kdamond 将执行的监测上下文的文件。

.. _sysfs_contexts_zh_CN:

kdamonds/<N>/contexts/
----------------------

开始时，该目录只有一个文件 ``nr_contexts``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个监测上下文（更多细节请参考 :doc:`设计文档 </mm/damon/design>`）。
目前，每个 kdamond 只支持一个上下文，因此只能向该文件写入 ``0`` 或 ``1``。

.. _sysfs_context_zh_CN:

contexts/<N>/
-------------

每个上下文目录中存在三个文件（``avail_operations``、``operations`` 和 ``addr_unit``）以及三个
目录（``monitoring_attrs``、``targets`` 和 ``schemes``）。

DAMON 支持多种监测操作，包括用于虚拟地址空间和物理地址空间的操作。读取 ``avail_operations``
文件可以获取当前运行内核中可用的监测操作集列表。根据内核配置，该文件会列出不同的可用操作集。
所有可用操作集及其简要说明请参考 :doc:`设计文档 </mm/damon/design>`。

你可以向 ``operations`` 文件写入 ``avail_operations`` 文件中列出的关键字之一，并从
``operations`` 文件读取，来设置和获取 DAMON 将为该上下文使用哪种监测操作。

``addr_unit`` 文件用于设置和获取操作集的地址单位参数。

.. _sysfs_monitoring_attrs_zh_CN:

contexts/<N>/monitoring_attrs/
------------------------------

用于指定监测属性（包括所需监测质量和效率）的文件位于 ``monitoring_attrs`` 目录中。具体来说，
该目录中存在两个目录：``intervals`` 和 ``nr_regions``。

在 ``intervals`` 目录下，存在三个 DAMON 间隔文件：采样间隔（``sample_us``）、聚集间隔
（``aggr_us``）和更新间隔（``update_us``）。你可以通过写入和读取这些文件来设置和获取以微秒为
单位的值。

在 ``nr_regions`` 目录下，存在两个用于 DAMON 监测区域下限和上限的文件（分别为 ``min`` 和
``max``），它们控制监测开销。你可以通过写入和读取这些文件来设置和获取这些值。

关于间隔和监测区域范围的更多细节，请参考设计文档（:doc:`/mm/damon/design`）。

.. _damon_usage_sysfs_monitoring_intervals_goal_zh_CN:

contexts/<N>/monitoring_attrs/intervals/intervals_goal/
-------------------------------------------------------

在 ``intervals`` 目录下，还存在一个用于自动调优 ``sample_us`` 和 ``aggr_us`` 的目录，即
``intervals_goal`` 目录。该目录下有四个用于自动调优控制的文件，即 ``access_bp``、``aggrs``、
``min_sample_us`` 和 ``max_sample_us``。关于调优机制的内部细节，请参考该功能的 :ref:`设计文档
<damon_design_monitoring_intervals_autotuning_zh_CN>`。读取和写入 ``intervals_goal`` 目录下的这四个
文件，会显示和更新 :ref:`设计文档 <damon_design_monitoring_intervals_autotuning_zh_CN>` 中描述的同名
调优参数。调优从用户设置的 ``sample_us`` 和 ``aggr_us`` 开始。向 ``state`` 文件写入
``update_tuned_intervals`` 后，可以从 ``sample_us`` 和 ``aggr_us`` 文件读取应用调优后的两个当前
间隔值。

.. _sysfs_targets_zh_CN:

contexts/<N>/targets/
---------------------

开始时，该目录只有一个文件 ``nr_targets``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个监测目标。

.. _sysfs_target_zh_CN:

targets/<N>/
------------

每个目标目录中存在两个文件（``pid_target`` 和 ``obsolete_target``）以及一个目录
（``regions``）。

如果你向 ``contexts/<N>/operations`` 写入了 ``vaddr``，则每个目标都应该是一个进程。你可以通过
向 ``pid_target`` 文件写入进程 pid 来指定 DAMON 要监测的进程。

用户可以向 ``obsolete_target`` 文件写入非零值并提交它（向 ``state`` 文件写入 ``commit``），
从目标数组中间选择性地删除目标。DAMON 会从它的内部目标数组中删除匹配的目标。用户负责重新构造
目标目录，使它们正确表示改变后的内部目标数组。

.. _sysfs_regions_zh_CN:

targets/<N>/regions
-------------------

对于 ``fvaddr`` 或 ``paddr`` 监测操作集，用户需要设置监测目标地址范围。对于 ``vaddr`` 操作集，
这不是强制要求，但用户可以选择性地将初始监测区域设置为特定地址范围。更多细节请参考
:ref:`设计文档 <damon_design_vaddr_target_regions_construction_zh_CN>`。

在这些情况下，用户可以按照自己的意愿，通过向该目录下的文件写入适当的值来显式设置初始监测目标
区域。

开始时，该目录只有一个文件 ``nr_regions``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个初始监测目标区域。

在线提交新的 DAMON 参数时（向 :ref:`kdamond <sysfs_kdamond_zh_CN>` 的 ``state`` 文件写入
``commit``），如果 ``nr_regions`` 为零，提交逻辑会忽略目标区域。换句话说，会保留该目标当前的
监测结果。

.. _sysfs_region_zh_CN:

regions/<N>/
------------

在每个区域目录中，你会看到两个文件（``start`` 和 ``end``）。你可以通过写入和读取这些文件，分
别设置和获取初始监测目标区域的起始地址和结束地址。

各区域之间不应重叠。目录 ``N`` 的 ``end`` 应小于或等于目录 ``N+1`` 的 ``start``。

.. _sysfs_schemes_zh_CN:

contexts/<N>/schemes/
---------------------

这是用于基于 DAMON 的操作方案（DAMON-based Operation Schemes，DAMOS）的目录。用户可以通过
读取和写入该目录下的文件来获取和设置方案。更多背景请参考 :doc:`设计文档 </mm/damon/design>`。

开始时，该目录只有一个文件 ``nr_schemes``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个基于 DAMON 的操作方案。

.. _sysfs_scheme_zh_CN:

schemes/<N>/
------------

每个方案目录中存在九个目录（``access_pattern``、``quotas``、``watermarks``、
``core_filters``、``ops_filters``、``filters``、``dests``、``stats`` 和 ``tried_regions``）
以及三个文件（``action``、``target_nid`` 和 ``apply_interval_us``）。

``action`` 文件用于设置和获取方案的动作。可写入和读取该文件的关键字及其含义与 :doc:`设计文档
</mm/damon/design>` 中的列表相同。

``target_nid`` 文件用于设置迁移目标节点。只有当 ``action`` 为 ``migrate_hot`` 或
``migrate_cold`` 时，该文件才有意义。

``apply_interval_us`` 文件用于以微秒为单位设置和获取方案的 ``apply_interval``。

.. _sysfs_access_pattern_zh_CN:

schemes/<N>/access_pattern/
---------------------------

该目录用于给定的基于 DAMON 的操作方案的目标访问模式。

在 ``access_pattern`` 目录下，存在三个目录（``sz``、``nr_accesses`` 和 ``age``），每个目录都有
两个文件（``min`` 和 ``max``）。你可以分别向 ``sz``、``nr_accesses`` 和 ``age`` 目录下的
``min`` 和 ``max`` 文件写入并读取，以设置和获取给定方案的访问模式。注意，``min`` 和 ``max`` 构
成闭区间。

.. _sysfs_quotas_zh_CN:

schemes/<N>/quotas/
-------------------

该目录用于给定的基于 DAMON 的操作方案的配额。

在 ``quotas`` 目录下，存在五个文件（``ms``、``bytes``、``reset_interval_ms``、
``effective_bytes`` 和 ``goal_tuner``）以及两个目录（``weights`` 和 ``goals``）。

你可以分别向这三个文件写入数值，设置以毫秒为单位的 ``时间配额``、以字节为单位的 ``大小配额``
以及以毫秒为单位的 ``重置间隔``。随后，DAMON 会尝试最多只使用 ``时间配额`` 毫秒，将 ``action``
应用于符合 ``access_pattern`` 的内存区域，并且在 ``reset_interval_ms`` 内最多只对 ``bytes`` 字
节的内存区域应用该动作。如果 ``ms`` 和 ``bytes`` 都设置为零，除非至少设置了一个 :ref:`目标
<sysfs_schemes_quota_goals_zh_CN>`，否则会禁用配额限制。

你可以通过向 ``goal_tuner`` 文件写入算法名称，设置要使用的基于目标的有效配额自动调优算法。读
取该文件会返回当前选择的调优器算法。关于该功能的背景设计和可选算法名称，请参考
:doc:`设计文档 </mm/damon/design>` 中的自动配额调优目标。关于目标设置，请参考 :ref:`goals 目录
<sysfs_schemes_quota_goals_zh_CN>`。

时间配额会在内部转换为大小配额。在转换后的大小配额和用户指定的大小配额之间，会应用较小者。基
于用户指定的 :ref:`目标 <sysfs_schemes_quota_goals_zh_CN>`，有效大小配额会被进一步调整。读取
``effective_bytes`` 会返回当前有效大小配额。该文件不会实时更新，因此用户应要求 DAMON sysfs 接
口更新该文件内容：向相关的 ``kdamonds/<N>/state`` 文件写入特殊关键字
``update_schemes_effective_quotas``。

在 ``weights`` 目录下，存在三个文件（``sz_permil``、``nr_accesses_permil`` 和 ``age_permil``）。
你可以通过向 ``weights`` 目录下的这三个文件写入数值，以千分之一为单位设置大小、访问频率和年龄
的优先级权重。

.. _sysfs_schemes_quota_goals_zh_CN:

schemes/<N>/quotas/goals/
-------------------------

该目录用于给定的基于 DAMON 的操作方案的自动配额调优目标。

开始时，该目录只有一个文件 ``nr_goals``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个目标及其当前达成情况。在多个反馈中，会使用最好的一个。

每个目标目录包含五个文件，即 ``target_metric``、``target_value``、``current_value``、``nid``
和 ``path``。用户可以通过写入和读取这些文件，设置和获取 :doc:`设计文档 </mm/damon/design>` 中
指定的配额自动调优目标的五个参数。注意，用户还应向 :ref:`kdamond 目录 <sysfs_kdamond_zh_CN>` 的
``state`` 文件写入 ``commit_schemes_quota_goals``，
以将反馈传递给 DAMON。

.. _sysfs_watermarks_zh_CN:

schemes/<N>/watermarks/
-----------------------

该目录用于给定的基于 DAMON 的操作方案的水位。

在 ``watermarks`` 目录下，存在五个文件（``metric``、``interval_us``、``high``、``mid`` 和
``low``），用于设置度量、检查该度量的时间间隔以及三个水位。你可以通过写入这些文件分别设置并获
取这五个值。

可写入 ``metric`` 文件的关键字及其含义如下。

 - none：忽略水位
 - free_mem_rate：系统空闲内存率（千分比）

``interval`` 应以微秒为单位写入。

.. _sysfs_filters_zh_CN:

schemes/<N>/{core\_,ops\_,}filters/
-----------------------------------

这些目录用于给定的基于 DAMON 的操作方案的过滤器。

``core_filters`` 和 ``ops_filters`` 目录分别用于由 DAMON 核心层和操作集层处理的过滤器。
``filters`` 目录可用于安装不区分处理层的过滤器。通过 ``core_filters`` 和 ``ops_filters`` 请求
的过滤器会在 ``filters`` 的过滤器之前安装。三个目录具有相同的文件。

使用 ``filters`` 目录时，通过目录下文件预期给定过滤器的求值顺序可能会有些混乱。因此，建议用户
使用 ``core_filters`` 和 ``ops_filters`` 目录。``filters`` 目录未来可能会被弃用。

开始时，该目录只有一个文件 ``nr_filters``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个过滤器。过滤器按数字顺序求值。

每个过滤器目录包含九个文件，即 ``type``、``matching``、``allow``、``memcg_path``、
``addr_start``、``addr_end``、``min``、``max`` 和 ``target_idx``。可以向 ``type`` 文件写入过滤
器类型。可用类型名称、含义以及它们由哪一层处理，请参考 :doc:`设计文档 </mm/damon/design>`。

对于 ``memcg`` 类型，可以通过向 ``memcg_path`` 文件写入从 cgroups 挂载点开始的内存 cgroup 路径
来指定关注的内存 cgroup。对于 ``addr`` 类型，可以分别向 ``addr_start`` 和 ``addr_end`` 文件写入
范围（开区间）的起始和结束地址。对于 ``hugepage_size`` 类型，可以分别向 ``min`` 和 ``max`` 文件
写入范围（闭区间）的最小和最大大小。对于 ``target`` 类型，可以向 ``target_idx`` 文件写入 DAMON
上下文监测目标列表中的目标索引。

可以向 ``matching`` 文件写入 ``Y`` 或 ``N``，指定过滤器是否用于匹配 ``type`` 的内存。可以向
``allow`` 文件写入 ``Y`` 或 ``N``，指定是否允许对满足 ``type`` 和 ``matching`` 的内存应用动作。

下面的示例将一个 DAMOS 动作限制为只应用于所有内存 cgroup 中的非匿名页，但排除
``/having_care_already``。::

    # cd ops_filters/0/
    # echo 2 > nr_filters
    # # disallow anonymous pages
    echo anon > 0/type
    echo Y > 0/matching
    echo N > 0/allow
    # # further filter out all cgroups except one at '/having_care_already'
    echo memcg > 1/type
    echo /having_care_already > 1/memcg_path
    echo Y > 1/matching
    echo N > 1/allow

更多细节，包括多个不同 ``allow`` 的过滤器如何工作、每个过滤器何时受支持以及统计信息差异，请参考
:doc:`DAMOS 过滤器设计文档 </mm/damon/design>`。

.. _damon_sysfs_dests_zh_CN:

schemes/<N>/dests/
------------------

该目录用于指定给定的基于 DAMON 的操作方案动作的目标。如果给定方案的动作不支持多个目标，则忽略
该目录。只有 ``DAMOS_MIGRATE_{HOT,COLD}`` 动作支持多个目标。

开始时，该目录只有一个文件 ``nr_dests``。向该文件写入一个数字（``N``）会创建名为 ``0`` 到
``N-1`` 的子目录。每个目录代表一个动作目标。

每个目标目录包含两个文件，即 ``id`` 和 ``weight``。用户可以向 ``id`` 文件写入目标标识符，也可
以从该文件读取目标标识符。对于 ``DAMOS_MIGRATE_{HOT,COLD}`` 动作，应向 ``id`` 文件写入迁移目
标节点的节点 id。用户可以向 ``weight`` 文件写入和读取给定目标之间的目标权重。权重可以是任意整
数。当 DAMOS 对内存区域中的每个实体应用动作时，它会基于这些目标的相对权重选择动作目标。

.. _sysfs_schemes_stats_zh_CN:

schemes/<N>/stats/
------------------

DAMON 会统计每个方案的信息。这些统计信息可用于方案的在线分析或调优。关于统计信息的更多细节，
请参考 :doc:`设计文档 </mm/damon/design>`。

可以通过读取 ``stats`` 目录下的文件分别检索这些统计信息：``nr_tried``、``sz_tried``、
``nr_applied``、``sz_applied``、``sz_ops_filter_passed``、``qt_exceeds``、``nr_snapshots`` 和
``max_nr_snapshots``。

这些文件默认不会实时更新。用户应要求 DAMON sysfs 接口使用 ``refresh_ms`` 周期性地更新这些文件，
或者通过向相关的 ``kdamonds/<N>/state`` 文件写入特殊关键字 ``update_schemes_stats`` 来执行一次
性更新。更多细节请参考 :ref:`kdamond 目录 <sysfs_kdamond_zh_CN>`。

.. _sysfs_schemes_tried_regions_zh_CN:

schemes/<N>/tried_regions/
--------------------------

该目录开始时有一个文件 ``total_bytes``。

当向相关的 ``kdamonds/<N>/state`` 文件写入特殊关键字 ``update_schemes_tried_regions`` 时，
DAMON 会更新 ``total_bytes`` 文件，使读取该文件返回方案尝试区域的总大小，并在该目录下创建从
``0`` 开始命名的整数目录。每个目录包含的文件会暴露对应方案的 ``action`` 在下一个对应方案的
应用间隔中，已经尝试应用到的每个内存区域的详细信息。该信息包括区域的地址范围、``nr_accesses``
和 ``age``。

向相关的 ``kdamonds/<N>/state`` 文件写入 ``update_schemes_tried_bytes`` 只会更新
``total_bytes`` 文件，而不会创建子目录。

当向相关的 ``kdamonds/<N>/state`` 文件写入另一个特殊关键字 ``clear_schemes_tried_regions`` 时，
这些目录会被删除。

该目录的预期用途是调查方案行为，以及像查询一样高效地检索数据访问监测结果。特别是对于后一种用
例，用户可以将 ``action`` 设置为 ``stat``，并将 ``access pattern`` 设置为他们想查询的感兴趣模
式。

.. _sysfs_schemes_tried_region_zh_CN:

tried_regions/<N>/
------------------

在每个区域目录中，你会看到五个文件（``start``、``end``、``nr_accesses``、``age`` 和
``sz_filter_passed``）。读取这些文件会显示对应的基于 DAMON 的操作方案 ``action`` 已经尝试应用
到的区域属性。

示例
~~~~

以下命令会应用一个方案，其含义是：“如果一个大小在 [4KiB, 8KiB] 之间的内存区域，在 [10, 20]
个聚集间隔内，每个聚集间隔显示 [0, 5] 次访问，则换出该区域。对于换出操作，每秒最多只使用 10ms，
并且每秒换出的内存不超过 1GiB。在这些限制下，优先换出年龄更长的内存区域。同时，每 5 秒检查一
次系统空闲内存率；当空闲内存率低于 50% 时开始监测和换出，但如果空闲内存率高于 60% 或低于 30%，
则停止。”::

    # cd <sysfs>/kernel/mm/damon/admin
    # # populate directories
    # echo 1 > kdamonds/nr_kdamonds; echo 1 > kdamonds/0/contexts/nr_contexts;
    # echo 1 > kdamonds/0/contexts/0/schemes/nr_schemes
    # cd kdamonds/0/contexts/0/schemes/0
    # # set the basic access pattern and the action
    # echo 4096 > access_pattern/sz/min
    # echo 8192 > access_pattern/sz/max
    # echo 0 > access_pattern/nr_accesses/min
    # echo 5 > access_pattern/nr_accesses/max
    # echo 10 > access_pattern/age/min
    # echo 20 > access_pattern/age/max
    # echo pageout > action
    # # set quotas
    # echo 10 > quotas/ms
    # echo $((1024*1024*1024)) > quotas/bytes
    # echo 1000 > quotas/reset_interval_ms
    # # set watermark
    # echo free_mem_rate > watermarks/metric
    # echo 5000000 > watermarks/interval_us
    # echo 600 > watermarks/high
    # echo 500 > watermarks/mid
    # echo 300 > watermarks/low

请注意，强烈建议使用像 `damo <https://github.com/damonitor/damo>`_ 这样的用户空间工具，而不是像
上面那样手动读取和写入文件。上面的内容只是一个示例。

.. _tracepoint_zh_CN:

监测结果的监测点
================

用户可以通过 :ref:`tried_regions <sysfs_schemes_tried_regions_zh_CN>` 获取监测结果。该接口对获
取快照很有用，但用于完整记录所有监测结果时可能效率较低。为此，DAMON 提供两个 tracepoint，即
``damon:damon_aggregated`` 和 ``damon:damos_before_apply``。``damon:damon_aggregated`` 提供
完整监测结果，而 ``damon:damos_before_apply`` 提供每个基于 DAMON 的操作方案（DAMOS）即将应用
到的区域的监测结果。因此，``damon:damos_before_apply`` 更适合记录 DAMOS 的内部行为，或者基于
DAMOS 目标访问模式进行类似查询的高效监测结果记录。

监测开启时，你可以记录 tracepoint 事件，并使用支持 tracepoint 的工具（如 ``perf``）显示结果。
例如::

    # echo on > kdamonds/0/state
    # perf record -e damon:damon_aggregated &
    # sleep 5
    # kill 9 $(pidof perf)
    # echo off > kdamonds/0/state
    # perf script
    kdamond.0 46568 [027] 79357.842179: damon:damon_aggregated: target_id=0 nr_regions=11 122509119488-135708762112: 0 864
    [...]

perf 脚本输出中的每一行代表一个监测区域。前五个字段与通常的 tracepoint 输出相同。第六个字段
（``target_id=X``）显示该区域所属监测目标的 id。第七个字段（``nr_regions=X``）显示该目标的监测
区域总数。第八个字段（``X-Y:``）显示该区域以字节为单位的起始地址（``X``）和结束地址（``Y``）。
第九个字段（``X``）显示该区域的 ``nr_accesses`` （关于该计数器的更多细节请参考 :ref:`设计文档
<damon_design_region_based_sampling_zh_CN>`）。最后，第十个字段（``X``）显示该区域的 ``age`` （关于该
计数器的更多细节请参考 :ref:`设计文档 <damon_design_age_tracking_zh_CN>`）。

如果事件是 ``damon:damos_before_apply``，perf 脚本输出大致如下::

    kdamond.0 47293 [000] 80801.060214: damon:damos_before_apply: ctx_idx=0 scheme_idx=0 target_idx=0 nr_regions=11 121932607488-135128711168: 0 136
    [...]

输出中的每一行代表在跟踪时刻每个基于 DAMON 的操作方案即将应用到的每个监测区域。前五个字段与
通常情况相同。除了 ``damon_aggregated`` tracepoint 的输出外，它还显示该方案所属 DAMON 上下文
在该上下文所属 kdamond 的上下文列表中的索引（``ctx_idx=X``），以及该方案在该上下文的方案列表
中的索引（``scheme_idx=X``）。
