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

Paravirt CPUs
=============

Under virtualised environments it is possible to overcommit CPU resources.
i.e sum of virtual CPU(vCPU) of all VM's is greater than number of physical
CPUs(pCPU). Under such conditions when all or many VM's have high utilization,
hypervisor won't be able to satisfy the CPU requirement and has to context
switch within or across VM. i.e hypervisor need to preempt one vCPU to run
another. This is called vCPU preemption. This is more expensive compared to
task context switch within a vCPU.

In such cases it is better that VM's co-ordinate among themselves and ask for
less CPU by not using some of the vCPUs. Such vCPUs where workload can be
avoided at the moment for less vCPU preemption are called as "Paravirt CPUs".
Note that when the pCPU contention goes away, these vCPUs can be used again
by the workload.

Arch need to set/unset the specific vCPU in cpu_paravirt_mask. When set, avoid
that vCPU and when unset, use it as usual.

Scheduler will try to avoid paravirt vCPUs as much as it can.
This is achieved by
1. Not selecting paravirt CPU at wakeup.
2. Push the task away from paravirt CPU at tick.
3. Not selecting paravirt CPU at load balance.

This works only for SCHED_RT and SCHED_NORMAL. SCHED_EXT and userspace can make
choices accordingly using cpu_paravirt_mask.

/sys/devices/system/cpu/paravirt prints the current cpu_paravirt_mask in
cpulist format.

Notes:
1. A task pinned only on paravirt CPUs will continue to run there.
2. This feature is available under CONFIG_PARAVIRT
3. Refer to PowerPC for architecure implementation side.
4. Doesn't push out any task running on isolated CPUs.

Possible arch/ problems
=======================

Possible arch problems I found (and either tried to fix or didn't):

sparc - IRQs on at this point(?), change local_irq_save to _disable.
      - TODO: needs secondary CPUs to disable preempt (See #1)
