================================
DAMON-based huge page collapsing
================================

DAMON-based huge page collapsing (SAMPLE_DAMON_HUGEPAGE) is a static kernel
module that aims to collapse hot regions into huge pages.

Where Proactive huge page collapsing is Required?
=================================================

The amount of available memory grows faster than the amount of TLB entries.
This leads to higher amount of TLB misses and excessive cycle wastes. Huge
pages are meant to solve this problem. However, huge pages usually lead to
memory fragmentation and memory waste.

Collapsing selectively hot regions in a specific process can avoid big
memory fragmentation, while increasing TLB performance.

SAMPLE_DAMON_HUGEPAGE solves this by:

- Identifying hot regions that have been accessed for a configured time
- Automatically collapsing the regions into huge pages back
- Auto tune the huge page usage ratio to meet desired targets
- Controlling the collapse rate with configurable quotas to avoid performance
  degradation


How It Works?
=============

SAMPLE_DAMON_HUGEPAGE uses kdamond to identify anonymous memory regions that
are:

1. Large enough to be backed by huge pages (``HPAGE_PMD_SIZE`` or larger)
2. Have been accessed for a configured time period

Once identified, SAMPLE_DAMON_HUGEPAGE triggers synchronous partial collapse
of those regions. The collapse operation is controlled by quotas to limit the
impact on system performance.

The module also supports automatic tuning of the collapse rate to achieve a
desired huge page usage ratio. Administrators can configure a target percentage
of huge page usage vs total anonymous memory usage.

Additionally, the module accepts manual feedback from system administrators to
adjust the effective quota level based on observed system behavior.

Interface: Module Parameters
============================

To use this feature, you should first ensure your system is running on a kernel
that is built with ``CONFIG_DAMON_HUGEPAGE=y``.

To let sysadmins enable or disable it and tune for the given system,
SAMPLE_DAMON_HUGEPAGE utilizes module parameters.  That is, you can put
``damon_sample_hugepage.<parameter>=<value>`` on the kernel boot command line or
write proper values to ``/sys/module/damon_sample_hugepage/parameters/<parameter>``
files.

Below are the description of each parameter.

enabled
-------

Enable or disable SAMPLE_DAMON_HUGEPAGE.

You can enable SAMPLE_DAMON_HUGEPAGE by setting the value of this parameter as ``Y``.
Setting it as ``N`` disables SAMPLE_DAMON_HUGEPAGE.  Note that SAMPLE_DAMON_HUGEPAGE
could do no real monitoring and collapse due to the activation condition.

quota_autotune_feedback
-----------------------

User-specifiable feedback for auto-tuning of the effective quota.

While keeping the caps that set by other quotas, SAMPLE_DAMON_HUGEPAGE
automatically increases and decreases the effective level of the quota
aiming receiving this feedback of value ``10,000`` from the user.
SAMPLE_DAMON_HUGEPAGE assumes the feedback value and the quota are positively
proportional.  Value zero means disabling this auto-tuning feature.

Disabled by default.

quota_percentage_hugepage
-------------------------

Huge page consumption to total memory anonymous memory consumption ratio goal
in bp ``(10,000)``. SAMPLE_DAMON_HUGEPAGE automatically increases and
decreases page collapse aggressiveness in order to achieve the given value.

target_pid
----------

PID of the task that is going to be monitored for hot regions.


Example
=======

Below runtime example commands make SAMPLE_DAMON_HUGEPAGE to find memory
regions of  the task with PID 1234 that have been accessed in the last 100
milliseconds or more and collpases those pages into huge pages. The page
collapsing is limited to be done only up to 1 GiB per second to avoid
SAMPLE_DAMON_HUGEPAGE consuming too much CPU time for the collapse operation. ::

    # cd /sys/module/damon_sample_hugepage/parameters
    # echo 1234 > target_pid
    # echo Y > enabled

Note that this module (SAMPLE_DAMON_HUGEPAGE) cannot run simultaneously
with other DAMON-based special-purpose modules.  Refer to
:ref:`DAMON design special purpose modules exclusivity
<damon_design_special_purpose_modules_exclusivity>` for more details.