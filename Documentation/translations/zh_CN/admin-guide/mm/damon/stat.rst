.. SPDX-License-Identifier: GPL-2.0
.. include:: ../../../disclaimer-zh_CN.rst

:Original: Documentation/admin-guide/mm/damon/stat.rst

:翻译:

 Doehyun Baek <doehyunbaek@gmail.com>

:校译:

======================
数据访问监测结果统计
======================

数据访问监测结果统计（DAMON_STAT）是一个静态内核模块，旨在用于简单的访问模式监测。它使用
DAMON 监测系统整块物理内存上的访问，并提供简化的访问监测结果统计信息，即空闲时间百分位数和
估计的内存带宽。

.. _damon_stat_monitoring_accuracy_overhead_zh_CN:

监测精度和开销
==============

DAMON_STAT 使用监测间隔 :ref:`自动调优 <damon_design_monitoring_intervals_autotuning_zh_CN>` 来提高
精度并最小化开销。它会自动调优间隔，目标是在每个快照中捕获 4 % 的可观测访问事件，同时将所得
采样间隔限制在最小 5 毫秒、最大 10 秒。在少数生产服务器系统上，它只消耗了 0.x % 的单 CPU
时间，同时捕获了质量合理的访问模式。调优得到的间隔可以通过 ``aggr_interval_us`` :ref:`参数
<damon_stat_aggr_interval_us_zh_CN>` 获取。

接口：模块参数
==============

要使用这个功能，首先应确保你的系统运行在构建时启用了 ``CONFIG_DAMON_STAT=y`` 的内核上。通过
将 ``CONFIG_DAMON_STAT_ENABLED_DEFAULT`` 设置为 true，可以在构建时默认启用该功能。

为了让系统管理员在启动时和/或运行时启用或禁用它，并读取监测结果，DAMON_STAT 提供了模块参数。
下面的章节描述各个参数。

enabled
-------

启用或禁用 DAMON_STAT。

你可以把该参数的值设置为 ``Y`` 来启用 DAMON_STAT。设置为 ``N`` 会禁用 DAMON_STAT。默认值由
``CONFIG_DAMON_STAT_ENABLED_DEFAULT`` 构建配置选项设置。

请注意，该模块（damon_stat）不能与其他基于 DAMON 的专用模块同时运行。更多细节请参考
:ref:`DAMON 设计文档的专用模块互斥性 <damon_design_special_purpose_modules_exclusivity_zh_CN>`。

.. _damon_stat_aggr_interval_us_zh_CN:

aggr_interval_us
----------------

自动调优后的聚集时间间隔，单位是微秒。

用户可以读取 DAMON_STAT 使用的 DAMON 实例的聚集间隔。该值会被 :ref:`自动调优
<damon_stat_monitoring_accuracy_overhead_zh_CN>`，因此会动态变化。

estimated_memory_bandwidth
--------------------------

系统的估计内存带宽消耗（字节/秒）。

DAMON_STAT 读取当前 DAMON 结果快照上的观测访问事件，并将其转换为以字节/秒为单位的内存带宽
消耗估计。该结果指标通过这个只读参数向用户公开。由于 DAMON 使用采样，所以这只是访问强度的估计，
而不是精确的内存带宽。

memory_idle_ms_percentiles
--------------------------

系统的逐字节空闲时间（毫秒）百分位数。

DAMON_STAT 基于当前 DAMON 结果快照，计算内存中每个字节到当前为止未被访问的时间（空闲时间）。
对于访问频率（nr_accesses）大于零的区域，当前访问频率级别保持了多久再乘以 ``-1``，就是该区域
每个字节的空闲时间。如果某个区域的访问频率（nr_accesses）为零，则该区域保持零访问频率的时间
（age）就是该区域每个字节的空闲时间。然后，DAMON_STAT 通过这个只读参数公开这些空闲时间值的
百分位数。读取该参数会返回 101 个以毫秒为单位、用逗号分隔的空闲时间值。每个值分别表示第 0、
第 1、第 2、第 3、……、第 99 和第 100 百分位的空闲时间。
