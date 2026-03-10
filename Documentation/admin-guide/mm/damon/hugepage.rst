.. SPDX-License-Identifier: GPL-2.0

=========================================
DAMON-based Huge Pages Collapsing
=========================================


DAMON-based dynamic hugepages collapsing (DAMON_HOT_HUGEPAGE) is a kernel module
that monitors a number of tasks simultaneously. The hot regions are collapsed
into huge pages.

Where Hot Region Huge Pages Collapsing is Required?
================================================

As main memory availability increases, the number of TLB entries does not
increase proportionally. This adds more pressure to TLB. Huge pages are a
solution. However, since turning on transparent huge pages globally may lead to
fragmentation and memory waste, it is usually turned off.

This module allows to automatically detect hot regions and collapse VMA that are
hot.

How It Works?
=============

DAMON_HOT_HUGEPAGE spawns a new kthread which will monitor the applications in
the system. The monitor thread will calculate the moving average of the sum of
utimes of all the threads for all the processes. Then, pick the top three and
launch a damon process to monitor the hot regions in those tasks.

Since we don't know the minaccess number in advance, we set it to 90 initially,
and we keep decreasing that minaccess until a collapse happens.

If a task turns cold, the monitor thread will detect it (it will not fall in the
top three hot tasks), and stop the damon thread for that task.

Interface: Module Parameters
============================

To use this feature, you should first ensure your system is running on a kernel
that is built with CONFIG_DAMON_HOT_HUGEPAGE=y.

To let sysadmins enable or disable it and tune for the given system,
DAMON_HOT_HUGEPAGE utilizes module parameters. That is, you can put
damon_dynamic_hotpages.<parameter>=<value> on the kernel boot command line or
write proper values to /sys/module/damon_dynamic_hotpages/parameters/<parameter>
files.

Below are the description of each parameter.

enabled
-------

Enable or disable DAMON_HOT_HUGEPAGE.

You can enable DAMON_HOT_HUGEPAGE by setting the value of this parameter as Y.
Setting it as N disables DAMON_HOT_HUGEPAGE. Note that, although
DAMON_HOT_HUGEPAGE monitors the system and starts damon threads, those threads
could do no real monitoring due to the watermarks-based activation condition.
Refer to below descriptions for the watermarks parameter for this.

quota_ms
--------

Limit of time for collapsing memory regions in milliseconds.

DAMON_HOT_HUGEPAGE tries to use only up to this time within a time window
(quota_reset_interval_ms) for trying memory collapse. This can be used for
limiting CPU consumption of DAMONHOT_HUGEPAGE. If the value is zero, the limit
is disabled.

10 ms by default.

quota_reset_interval_ms
-----------------------

The time quota charge reset interval in milliseconds.

The charge reset interval for the quota of time (quota_ms). That is,
DAMON_HOT_HUGEPAGE does not collapse memory for more than quota_ms milliseconds
or quotasz bytes within quota_reset_interval_ms milliseconds.

1 second by default.

wmarks_interval
---------------

The watermarks check time interval in microseconds.

Minimal time to wait before checking the watermarks, when DAMON_HOT_HUGEPAGE is
enabled but inactive due to its watermarks rule. 5 seconds by default.

wmarks_high
-----------

Free memory rate (per thousand) for the high watermark.

If free memory of the system in bytes per thousand bytes is higher than this,
DAMON_HOT_HUGEPAGE becomes inactive, so it does nothing but periodically checks
the watermarks. 200 (20%) by default.

wmarks_mid
----------

Free memory rate (per thousand) for the middle watermark.

If free memory of the system in bytes per thousand bytes is between this and the
low watermark, DAMON_HOT_HUGEPAGE becomes active, so starts the monitoring and
the memory collapsing. 150 (15%) by default.

wmarks_low
----------

Free memory rate (per thousand) for the low watermark.

If free memory of the system in bytes per thousand bytes is lower than this,
DAMON_HOT_HUGEPAGE becomes inactive, so it does nothing but periodically checks
the watermarks. 50 (5%) by default.

sample_interval
---------------

Sampling interval for the monitoring in microseconds.

The sampling interval of DAMON for the cold memory monitoring. Please refer to
the DAMON documentation (:doc:usage) for more detail. 5ms by default.

aggr_interval
-------------

Aggregation interval for the monitoring in microseconds.

The aggregation interval of DAMON for the cold memory monitoring. Please refer
to the DAMON documentation (:doc:usage) for more detail. 100ms by default.

min_nr_regions
--------------

Minimum number of monitoring regions.

The minimal number of monitoring regions of DAMON for the cold memory
monitoring. This can be used to set lower-bound of the monitoring quality. But,
setting this too high could result in increased monitoring overhead. Please
refer to the DAMON documentation (:doc:usage) for more detail. 10 by default.

max_nr_regions
--------------

Maximum number of monitoring regions.

The maximum number of monitoring regions of DAMON for the cold memory
monitoring. This can be used to set upper-bound of the monitoring overhead.
However, setting this too low could result in bad monitoring quality. Please
refer to the DAMON documentation (:doc:usage) for more detail. 1000 by defaults.

monitored_pids
--------------

List of the tasks that are being actively monitored by DAMON threads.

commit_inputs
-------------

Make DAMON_HOT_HUGEPAGE reads the input parameters again, except ``enabled``.

Input parameters that updated while DAMON_HOT_HUGEPAGE is running are not
applied by default. Once this parameter is set as ``Y``, DAMON_HOT_HUGEPAGE
reads values of parametrs except ``enabled`` again. Once the re-reading is done,
this parameter is set as ``N``.  If invalid parameters are found while the
re-reading, DAMON_HOT_HUGEPAGE will be disabled.

Example
=======
Below runtime example commands make DAMON_HOT_HUGEPAGE to find memory regions in
the 3 tasks. It also asks DAMON_HOT_HUGEPAGE to do nothing if the system's free
memory rate is more than 50%, but start the real works if it becomes lower than
40%.

    # cd /sys/module/damon_dynamic_hotpages/parameters/
    # echo 3811,10123,12111 > monitored_pids
    # echo 10 > quota_ms
    # echo 1000 > quota_reset_interval_ms
    # echo 500 > wmarks_high
    # echo 400 > wmarks_mid
    # echo 200 > wmarks_low
    # echo Y > enabled