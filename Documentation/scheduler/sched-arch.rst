=================================================================
CPU Scheduler implementation hints for architecture specific code
=================================================================

	Nick Piggin, 2005

Context switch
==============
1. Runqueue locking
By default, the switch_to arch function is called with the runqueue
locked. This is usually not a problem unless switch_to may need to
take the runqueue lock. This is usually due to a wake up operation in
the context switch.

To request the scheduler call switch_to with the runqueue unlocked,
you must `#define __ARCH_WANT_UNLOCKED_CTXSW` in a header file
(typically the one where switch_to is defined).

Unlocked context switches introduce only a very minor performance
penalty to the core scheduler implementation in the CONFIG_SMP case.

CPU idle
========
Your cpu_idle routines need to obey the following rules:

1. Preempt should now disabled over idle routines. Should only
   be enabled to call schedule() then disabled again.

2. need_resched/TIF_NEED_RESCHED is only ever set, and will never
   be cleared until the running task has called schedule(). Idle
   threads need only ever query need_resched, and may never set or
   clear it.

3. When cpu_idle finds (need_resched() == 'true'), it should call
   schedule(). It should not call schedule() otherwise.

4. The only time interrupts need to be disabled when checking
   need_resched is if we are about to sleep the processor until
   the next interrupt (this doesn't provide any protection of
   need_resched, it prevents losing an interrupt):

	4a. Common problem with this type of sleep appears to be::

	        local_irq_disable();
	        if (!need_resched()) {
	                local_irq_enable();
	                *** resched interrupt arrives here ***
	                __asm__("sleep until next interrupt");
	        }

5. TIF_POLLING_NRFLAG can be set by idle routines that do not
   need an interrupt to wake them up when need_resched goes high.
   In other words, they must be periodically polling need_resched,
   although it may be reasonable to do some background work or enter
   a low CPU priority.

      - 5a. If TIF_POLLING_NRFLAG is set, and we do decide to enter
	an interrupt sleep, it needs to be cleared then a memory
	barrier issued (followed by a test of need_resched with
	interrupts disabled, as explained in 3).

arch/x86/kernel/process.c has examples of both polling and
sleeping idle functions.

Preferred CPUs
==============

In virtualised environments it is possible to overcommit CPU resources.
i.e sum of virtual CPU(vCPU) of all VM's is greater than number of physical
CPUs(pCPU). Under such conditions when all or many VM's have high utilization,
hypervisor won't be able to satisfy the CPU requirement and has to context
switch within or across VM. i.e hypervisor need to preempt one vCPU to run
another. This is called vCPU preemption. This is more expensive compared to
task context switch within a vCPU.

In such cases it is better that VM's co-ordinate among themselves and ask for
less CPU by not using some of the vCPUs. vCPUs where workload can be safely
scheduled which won't increase any contention for pCPU are called as
"Preferred CPUs".

In most cases preferred CPUs will be same as online CPUs, when there is pCPU
contention, Preferred CPUs will reduce based on the amount of steal time.
When the pCPU contention goes away as indicated by steal time, Preferred CPUs
will become same as online CPUs again. One has to enable the feature by
writing 1 to /sys/kernel/debug/sched/steal_monitor/enable

One of the design construct is preferred CPUs is always subset of online CPUs.
With CONFIG_PREFERRED_CPU=n, it is same as online CPUs.

For scheduling decisions such as wakeup, pushing the task etc, needs this
CPU state info. This is maintained in cpu_preferred_mask.

vCPUs which are not in cpu_preferred_mask should be treated as vCPUs which
should not be used at this moment provided it doesn't break user affinity.
This is achieved by
1. Selecting only a preferred CPU at wakeup.
2. Push the task away from non-preferred CPU at tick.
3. Only select preferred CPUs for load balance.

/sys/devices/system/cpu/preferred prints the current cpu_preferred_mask in
cpulist format.

Notes:
1. This feature is available under CONFIG_PREFERRED_CPU
2. This feature works for FAIR/RT class.
3. A task pinned, which can't be moved to preferred CPUs will continue
   to run based on its affinity. But no load balancing happens
4. If needed, steal time based governors/arch dependent method
   could be used to cater to different types of cpu numbers.
   Arch can do so by implementing its own hooks.
5. Decision to use/not use is driven by kernel. Hence it shouldn't
   break user affinities. One of the main reason why CPU hotplug
   or Isolated cpuset partitions was not a solution.

Possible arch/ problems
=======================

Possible arch problems I found (and either tried to fix or didn't):

sparc - IRQs on at this point(?), change local_irq_save to _disable.
      - TODO: needs secondary CPUs to disable preempt (See #1)
