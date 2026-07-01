.. SPDX-License-Identifier: GPL-2.0
=============
Steal Monitor
=============

:Author: Shrikanth Hegde

Introduction
============

Steal monitor is a driver aimed at solving the Noisy Neighbour problem
in virtualized environments. I.e performance of workload
running in one VM gets affected significantly due to other VMs and
combined they make slower forward progress.

When there is overcommit of CPU resources, i.e sum of virtual CPUs(vCPU)
of all VMs is greater than number of physical CPUs(pCPU) and
when all or many VMs have high utilization, hypervisor won't be able
to satisfy the CPU requirement and has to context switch within or
across VM. I.e hypervisor needs to preempt one vCPU to run
another. This is called vCPU preemption.
This is more expensive compared to task context switch within a vCPU.

In such cases it is better that combined vCPU ask from all VM is reduced
by not using some of the vCPUs. vCPUs where workload can be safely
scheduled which won't increase any contention for pCPU are called as
"Preferred CPUs".

See more on "Preferred CPUs" in Documentation/scheduler/sched-arch.rst.

This driver helps in setting/clearing the CPUs in the "Preferred CPUs" list.
This list is obtained using cpu_preferred_mask.

Core idea
=========
steal time is an indication available today in Guest which shows contention
for underlying physical CPU. Use it as a hint in the guest to fold the
workload to a reduced set of vCPUs. When there is contention, steal time
will show up in all the guests. When each guest honors the hint and folds
the workload to a smaller set of vCPUs(Preferred CPUs), it reduces the
contention and thereby reduces vCPU preemption.
This is achieved without any cross-guest communication.

Steal monitor driver effectively does:

1. Periodically computes steal time across the system.

2. If steal time is greater than high threshold, reduce the number of
   preferred CPUs by 1 core. Ensure at least one core is left always.
   This avoids running into extreme cases.

3. If steal time is lower or equal to low threshold, increase the
   number of preferred CPUs by 1 core. If preferred is same as active,
   nothing to be done.

4. Ensure preferred CPUs is always subset of active CPUs.
   On feature disable it is same as active CPUs.

This feature works best only when all the VMs enable the feature as
it is a co-operative scheme. If a specific VM don't enable this feature
it may end up with more CPUs than others, still should lead to better
performance when seen from system view.
Ones who are enabling this driver has to ensure it is enabled in all VMs.

Module Parameters
=================
interval_ms
-----------
How often steal monitor checks for steal time.
(Default: 1000 i.e 1 second)

This controls how fast steal monitor driver reacts to changes to
the contention of physical CPUs. Since it does fair amount of
work, setting too low will have overheads. If set to 0, on next
work it will be set to default.

low_threshold
-------------
lower threshold value in percentage * 100.
(Default: 200, i.e 2% steal is considered as low threshold)

This determines what values should be considered as nil/no steal values.
When steal monitor see steal time is below or equal to this value, it
will increase the preferred CPUs by 1 core. Having value as zero
might cause too much oscillations.

high_threshold
--------------
higher threshold value in percentage * 100
(Default: 500, i.e 5% steal is considered as high threshold)

This determines what values should be considered as high steal values.
When steal monitor sees steal time is higher than this value, it will
reduce the preferred CPUs by 1 core.

Notes
=====
This is available under CONFIG_PREFERRED_CPU. Selecting that includes
this module. Module is not loaded by default.
