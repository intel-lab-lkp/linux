.. SPDX-License-Identifier: GPL-2.0

==============
Steal Governor
==============

:Author: Shrikanth Hegde <sshegde@linux.ibm.com>

Introduction
============

Steal governor is a driver aimed at solving the Noisy Neighbour problem
in paravirtualized environments. The performance of workload
running in one VM gets affected significantly due to other VMs and
combined they make slower forward progress.

When there is overcommit of CPU resources, i.e. sum of virtual CPUs (vCPUs)
of all VMs is greater than number of physical CPUs (pCPUs) and
when all or many VMs have high utilization, hypervisor won't be able
to satisfy the CPU requirement and has to context switch within or
across VMs. I.e. the hypervisor needs to preempt one vCPU to run
another. This is called vCPU preemption.
This is more expensive compared to task context switch within a vCPU.

In such cases it is better that combined vCPU ask from all VMs is reduced
by not using some of the vCPUs. vCPUs where workload can be safely
scheduled which won't increase any contention for pCPU are called as
"Preferred CPUs".

See more on "Preferred CPUs" in Documentation/scheduler/sched-arch.rst.
Driver code is available at drivers/virt/steal_governor.c

This driver makes CONFIG_PREFERRED_CPU=y which enables the scheduler core
infrastructure to move tasks to Preferred CPUs where possible.

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

1. Periodically computes steal time across the system.

2. If steal time is greater than high threshold, reduce the number of
   preferred CPUs by 1 core. Ensure at least one core is left always.

3. If steal time is lower or equal to low threshold, increase the
   number of preferred CPUs by 1 core. If preferred is same as active,
   nothing to be done.

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
Default: 1000 i.e 1 second. Value should be in between 100ms to 100sec.

This controls how fast steal governor driver reacts to changes to
the contention of physical CPUs. Since it does a fair amount of
work, setting too low may have overhead. Setting it too
high might render it ineffective.

low_threshold
-------------

lower threshold value in percentage * 100.
Default: 200, i.e 2% steal is considered as low threshold.
Can't be higher than high_threshold.

This determines what values should be considered as nil/no steal values.
When steal governor see steal time is below or equal to this value, it
will increase the preferred CPUs by 1 core. Having value as zero
might cause oscillations.

high_threshold
--------------

higher threshold value in percentage * 100
Default: 500, i.e 5% steal is considered as high threshold.
Can't be lower than low_threshold. Must be less than 10000.

This determines what values should be considered as high steal values.
When steal governor sees steal time is higher than this value, it will
reduce the preferred CPUs by 1 core.

Notes
=====

Selecting this driver makes CONFIG_PREFERRED_CPU=y. That makes configs
driven by user preference.

It is recommended to build CONFIG_STEAL_GOVERNOR=m due to below reasons:

1. Doing periodic work has additional overheads. Enabling this driver
   in systems where steal time cannot happen is of no use. There is no
   benefit with additional overheads in such systems.

2. This works well when all VMs work in co-operative manner. When an
   administrative user enables it in one VM, he/she will likely enable
   it all VMs.
