.. SPDX-License-Identifier: GPL-2.0

Steal Governor
==============

:Author: Shrikanth Hegde <sshegde@linux.ibm.com>

Introduction
============

The steal governor is aimed at mitigating the Noisy Neighbour problem
which occurs in paravirtualized environments with CPU overcommit.
The performance of a workload running in one VM gets degraded by
the activity of other VMs on the same host. As a result, all VMs
collectively make slower forward progress.

In such systems, high utilization in all VMs causes the hypervisor to
frequently preempt vCPUs. This vCPU preemption is expensive.
To mitigate this, the kernel aims to restrict workloads to a subset of
Preferred CPUs to reduce physical CPU contention.
A detailed explanation of Preferred CPUs is available in
``Documentation/scheduler/sched-paravirt.rst``.

The steal governor selects ``CONFIG_PREFERRED_CPU=y`` which enables the
scheduler core infrastructure to move the tasks to Preferred CPUs where
possible. The driver controls the policy decisions regarding the state of
preferred CPUs. That is, this driver decides which CPUs are preferred
and which CPUs are non-preferred.

The driver code is available at ``drivers/virt/steal_governor.c``.

Core idea
=========

steal time is an indication available today in Guest which shows contention
for underlying physical CPU. Use it as a hint in the guest to fold the
workload to a reduced set of vCPUs. When there is contention, steal time
will show up in all the guests. When each guest honors the hint and folds
the workload to a smaller set of vCPUs (Preferred CPUs), it reduces the
contention and thereby reduces vCPU preemption.
This is achieved without any cross-guest communication.

Steal governor driver effectively does:

1. Periodically computes steal ratio across the possible CPUs.

2. If steal ratio is greater than high threshold, reduce the number of
   preferred CPUs by 1 core. Ensure at least one core is left always.
   Skip changing the state of offline CPUs in that core.

3. If steal ratio is less than or equal to low threshold, increase the
   number of preferred CPUs by 1 core. If preferred is same as active,
   nothing to be done. Skip changing the state of offline CPUs.
   This helps to handle cases where few CPUs are offline in a core and
   those offline CPUs will not be marked as preferred.

4. Ensure preferred CPUs is always subset of active CPUs.
   On feature disable it is same as active CPUs.

This feature works best only when all the VMs enable the feature as
it is a co-operative scheme. If a specific VM doesn't enable this feature
it may end up with more CPUs than others, still should lead to better
performance when seen from system view.
Those who enable this driver must ensure it is enabled in all VMs.

Module Parameters
=================

interval_ms
-----------

How often steal governor checks for steal time.
Default: 1000 i.e. 1 second. Value should be in between 100ms to 100sec.

This controls how fast steal governor driver reacts to changes to the
contention of physical CPUs. Since it does a fair amount of work, setting
too low may have overhead. Setting it too high might render it ineffective.

low_threshold
-------------

lower threshold value in percentage * 100.
Default: 200, i.e. 2% steal is considered as low threshold.
Can't be higher than high_threshold.

This determines what values should be considered as nil/no steal values.
When steal governor sees steal ratio is less than or equal to this value,
it will increase the preferred CPUs by 1 core.
Using zero might cause oscillations.

high_threshold
--------------

higher threshold value in percentage * 100
Default: 500, i.e. 5% steal is considered as high threshold.
Can't be lower than low_threshold. Must be less than 10000.

This determines what values should be considered as high steal values.
When steal governor sees steal ratio is higher than this value, it will
reduce the preferred CPUs by 1 core.

Limitations of default values
-----------------------------

Because of the vast diversity in VM configurations (e.g., highly populated
vs. sparsely populated CPU masks, few offlined CPUs etc), the default
thresholds may not be optimal for all systems. Users may need to tune these
parameters based on the system under test to achieve the best results.

For example:
Possible CPUs = 128 and Active CPUs = 8
Steal on online CPUs = 50%
steal ratio: (50% * 8 + 0% * 120) / 128 = 3.125%
This would fall in between and default values won't work.
In this example, if one wants effective 2% and 5% limits, then set,
low_threshold  = (2% * 8 + 0% * 120) / 128 = 0.1250% = 12
high_threshold = (5% * 8 + 0% * 120) / 128 = 0.3125% = 31

Using possible CPUs helps to handle spikes during CPU hotplug as the steal
time across possible CPUs is a monotonically increasing value.

Reasons for CONFIG_STEAL_GOVERNOR=m
===================================

Selecting this driver makes CONFIG_PREFERRED_CPU=y. That makes configs
driven by user preference. Though one can have CONFIG_STEAL_GOVERNOR=y,
It is recommended to build CONFIG_STEAL_GOVERNOR=m due to below reasons:

1. Doing periodic work has additional overheads. Enabling this driver
   in systems where steal time cannot happen is of no use. There is no
   benefit with additional overheads in such systems.

2. This works well when all VMs work in co-operative manner. When an
   administrative user enables it in one VM, he/she will likely enable
   it all VMs.

3. User can tweak the module parameters by reloading the module.
