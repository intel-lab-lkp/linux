.. SPDX-License-Identifier: GPL-2.0

=================
Workingset Report
=================
Workingset report provides a view of memory coldness in user-defined
time intervals, i.e. X bytes are Y milliseconds cold. It breaks down
the user pages in the system per-NUMA node, per-memcg, for both
anonymous and file pages into histograms that look like:
::

    1000 anon=137368 file=24530
    20000 anon=34342 file=0
    30000 anon=353232 file=333608
    40000 anon=407198 file=206052
    9223372036854775807 anon=4925624 file=892892

The workingset reports can be used to drive proactive reclaim, by
identifying the number of cold bytes in a memcg, then writing to
``memory.reclaim``.

Quick start
===========
Build the kernel with the following configurations. The report relies
on Multi-gen LRU for page coldness.

* ``CONFIG_LRU_GEN=y``
* ``CONFIG_LRU_GEN_ENABLED=y``
* ``CONFIG_WORKINGSET_REPORT=y``

Optionally, the aging kernel daemon can be enabled with the following
configuration.
* ``CONFIG_WORKINGSET_REPORT_AGING=y``

Sysfs interfaces
================
``/sys/devices/system/node/nodeX/workingset_report/page_age`` provides
a per-node page age histogram, showing an aggregate of the node's lruvecs.
Reading this file causes a hierarchical aging of all lruvecs, scanning
pages and creates a new Multi-gen LRU generation in each lruvec.
For example:
::

    1000 anon=0 file=0
    2000 anon=0 file=0
    100000 anon=5533696 file=5566464
    18446744073709551615 anon=0 file=0

``/sys/devices/system/node/nodeX/workingset_report/page_age_intervals``
is a comma separated list of time in milliseconds that configures what
the page age histogram uses for aggregation. For the above histogram,
the intervals are:
::
    1000,2000,100000

``/sys/devices/system/node/nodeX/workingset_report/refresh_interval``
defines the amount of time the report is valid for in milliseconds.
When a report is still valid, reading the ``page_age`` file shows
the existing valid report, instead of generating a new one.

``/sys/devices/system/node/nodeX/workingset_report/report_threshold``
specifies how often the userspace agent can be notified for node
memory pressure, in milliseconds. When a node reaches its low
watermarks and wakes up kswapd, programs waiting on ``page_age`` are
woken up so they can read the histogram and make policy decisions.

Memcg interface
===============
While ``page_age_interval`` is defined per-node in sysfs, ``page_age``,
``refresh_interval`` and ``report_threshold`` are available per-memcg.

``/sys/fs/cgroup/.../memory.workingset.page_age``
The memcg equivalent of the sysfs workingset page age histogram
breaks down the workingset of this memcg and its children into
page age intervals. Each node is prefixed with a node header and
a newline. Non-proactive direct reclaim on this memcg can also
wake up userspace agents that are waiting on this file.
e.g.
::

    N0
    1000 anon=0 file=0
    2000 anon=0 file=0
    3000 anon=0 file=0
    4000 anon=0 file=0
    5000 anon=0 file=0
    18446744073709551615 anon=0 file=0

``/sys/fs/cgroup/.../memory.workingset.refresh_interval``
The memcg equivalent of the sysfs refresh interval. A per-node
number of how much time a page age histogram is valid for, in
milliseconds.
e.g.
::

    echo N0=2000 > memory.workingset.refresh_interval

``/sys/fs/cgroup/.../memory.workingset.report_threshold``
The memcg equivalent of the sysfs report threshold. A per-node
number of how often userspace agent waiting on the page age
histogram can be woken up, in milliseconds.
e.g.
::

    echo N0=1000 > memory.workingset.report_threshold
