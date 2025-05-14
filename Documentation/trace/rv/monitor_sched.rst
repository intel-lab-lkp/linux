Scheduler monitors
==================

- Name: sched
- Type: container for multiple monitors
- Author: Gabriele Monaco <gmonaco@redhat.com>, Daniel Bristot de Oliveira <bristot@kernel.org>

Description
-----------

Monitors describing complex systems, such as the scheduler, can easily grow to
the point where they are just hard to understand because of the many possible
state transitions.
Often it is possible to break such descriptions into smaller monitors,
sharing some or all events. Enabling those smaller monitors concurrently is,
in fact, testing the system as if we had one single larger monitor.
Splitting models into multiple specification is not only easier to
understand, but gives some more clues when we see errors.

The sched monitor is a set of specifications to describe the scheduler behaviour.
It includes several per-cpu and per-task monitors that work independently to verify
different specifications the scheduler should follow.

To make this system as straightforward as possible, sched specifications are *nested*
monitors, whereas sched itself is a *container*.
From the interface perspective, sched includes other monitors as sub-directories,
enabling/disabling or setting reactors to sched, propagates the change to all monitors,
however single monitors can be used independently as well.

It is important that future modules are built after their container (sched, in
this case), otherwise the linker would not respect the order and the nesting
wouldn't work as expected.
To do so, simply add them after sched in the Makefile.

Specifications
--------------

The specifications included in sched are currently a work in progress, adapting the ones
defined in by Daniel Bristot in [1].

Currently we included the following:

Monitor sco
~~~~~~~~~~~

The scheduling context operations (sco) monitor ensures changes in a task state
happen only in thread context, the only exception is a special kind of set
state that occurs if a task about to sleep has a pending signal. This set state
is not called by the thread but by the scheduler itself::

                                        |
                                        |
                                        v
    sched_set_state                   +------------------+
  +---------------------------------- |                  |
  |                                   |  thread_context  |
  +---------------------------------> |                  | <+
                                      +------------------+  |
                                        |                   |
                                        | schedule_entry    | schedule_exit
                                        v                   |
    sched_set_state_runnable_signal                         |
  +----------------------------------                       |
  |                                    scheduling_context   |
  +--------------------------------->                      -+

Monitor snroc
~~~~~~~~~~~~~

The set non runnable on its own context (snroc) monitor ensures changes in a
task state happens only in the respective task's context. This is a per-task
monitor. A task is in its own context after switching in and leaves the context
when switched out, a vain switch maintains the context::

                          |
                          |
                          v
                        +------------------+
                        |  other_context   | <+
                        +------------------+  |
                          |                   |
                          | sched_switch_in   | sched_switch_out
                          v                   |
    sched_set_state                           |
    sched_switch_vain                         |
  +--------------------     own_context       |
  |                                           |
  +------------------->                      -+

Monitor scpd
~~~~~~~~~~~~

The schedule called with preemption disabled (scpd) monitor ensures schedule is
called with preemption disabled::

                       |
                       |
                       v
                     +------------------+
                     |    cant_sched    | <+
                     +------------------+  |
                       |                   |
                       | preempt_disable   | preempt_enable
                       v                   |
    schedule_entry                         |
    schedule_exit                          |
  +-----------------      can_sched        |
  |                                        |
  +---------------->                      -+

Monitor snep
~~~~~~~~~~~~

The schedule does not enable preempt (snep) monitor ensures a schedule call
does not enable preemption::

                        |
                        |
                        v
    preempt_disable   +------------------------+
    preempt_enable    |                        |
  +------------------ | non_scheduling_context |
  |                   |                        |
  +-----------------> |                        | <+
                      +------------------------+  |
                        |                         |
                        | schedule_entry          | schedule_exit
                        v                         |
                                                  |
                          scheduling_contex      -+

Monitor sncid
~~~~~~~~~~~~~

The schedule not called with interrupt disabled (sncid) monitor ensures
schedule is not called with interrupt disabled::

                       |
                       |
                       v
    schedule_entry   +--------------+
    schedule_exit    |              |
  +----------------- |  can_sched   |
  |                  |              |
  +----------------> |              | <+
                     +--------------+  |
                       |               |
                       | irq_disable   | irq_enable
                       v               |
                                       |
                        cant_sched    -+

Monitor sts
~~~~~~~~~~~

The schedule implies task switch (sts) monitor ensures a task switch happens in
every scheduling context, that is inside a call to ``__schedule``, as well as no
task switch can happen without scheduling and before interrupts are disabled.
This require the special type of switch called vain, which occurs when the next
task picked for execution is the same as the previously running one, in fact no
real task switch occurs::

                    |
                    |
                    v
                  #====================#   irq_disable
                  H                    H   irq_enable
                  H       thread       H --------------+
                  H                    H               |
  +-------------> H                    H <-------------+
  |               #====================#
  |                 |
  |                 | schedule_entry
  |                 v
  |               +--------------------+
  |               |     scheduling     | <+
  |               +--------------------+  |
  |                 |                     |
  |                 | irq_disable         | irq_enable
  |                 v                     |
  |               +--------------------+  |
  |               | disable_to_switch  | -+
  | schedule_exit +--------------------+
  |                 |
  |                 | sched_switch
  |                 | sched_switch_vain
  |                 v
  |               +--------------------+
  |               |     switching      |
  |               +--------------------+
  |                 |
  |                 | irq_enable
  |                 v
  |               +--------------------+   irq_disable
  |               |                    |   irq_enable
  |               |   enable_to_exit   | --------------+
  |               |                    |               |
  +-------------- |                    | <-------------+
                  +--------------------+

Monitor nrp
-----------

The need resched preempts (nrp) monitor ensures preemption requires need
resched. A preemption is any of the following types of ``sched_switch``:

* ``sched_switch_preempt``:
  a task is ``preempted``, this can happen after the need for ``rescheduling``
  has been set, also in its ``lazy`` flavour (which doesn't make a different in
  this monitor). ``need_resched`` can be set as a flag to the task or in the
  per-core preemption count, either of them can trigger a preemption.
* ``sched_switch_vain_preempt``:
  a task goes through the scheduler from a preemption context but it is picked
  as the next task to run, hence no real task switch occurs. Since we run the
  scheduler, this clears the need to reschedule.

This monitor ignores when the task is switched in, as this complicates things
when different types of ``sched_switch`` occur (e.g. sleeping or yielding, here
marked as ``sched_switch_other`` or ``sched_switch_vain``). The snroc monitor
ensures a task is switched in before it can be switched out again.
For this reason, the ``any_thread_running`` state does not imply that the
monitored task is not running, simply it is not set for rescheduling::

                           |
                           |
                           v
    sched_switch_other   #=====================#
    sched_switch_vain    H                     H
  +--------------------- H any_thread_running  H
  |                      H                     H
  +--------------------> H                     H <+
                         #=====================#  |
                           |                      |
                           |                      | sched_switch_preempt
                           |                      | sched_switch_vain_preempt
                           | sched_need_resched   | sched_switch_other
                           |                      | sched_switch_vain
                           v                      |
    sched_need_resched   +---------------------+  |
  +--------------------- |                     |  |
  |                      |    rescheduling     |  |
  +--------------------> |                     | -+
                         +---------------------+

Monitor sssw
------------

The set state sleep and wakeup (sssw) monitor ensures ``sched_set_state`` to
sleepable leads to sleeping and sleeping tasks require wakeup.
It includes the following types of ``sched_switch``:

* ``switch_suspend``:
  a task puts itself to sleep, this can happen only after explicitly setting
  the task to ``sleepable``. After a task is suspended, it needs to be woken up
  (``waking`` state) before being switched in again.
  Setting the task's state to ``sleepable`` can be reverted before switching if it
  is woken up or set to ``runnable``.
* ``switch_blocked``:
  a special case of a ``switch_suspend`` where the task is waiting on a
  sleeping RT lock (``PREEMPT_RT`` only), it is common to see wakeup and set
  state events racing with each other and this leads the model to perceive this
  type of switch when the task is not set to sleepable. This is a limitation of
  the model in SMP system and workarounds may slow down the system.
* ``switch_yield``:
  a task explicitly calls the scheduler, this looks like a preemption as the
  task is still runnable but the ``need_resched`` flag is not set. It can
  happen after a ``yield`` system call or from the idle task. By definition,
  a task cannot yield while ``sleepable`` as that would be a suspension.
* ``switch_vain``:
  a task explicitly calls the scheduler but it is picked as the next task to run,
  hence no real task switch occurs. This can occur as a yield, which is not
  valid when the task is sleepable. A special case of a yield is when a task in
  ``TASK_INTERRUPTIBLE`` calls the scheduler while a signal is pending. The
  task doesn't go through the usual blocking/waking and is set back to
  runnable, the resulting switch looks like a yield.

As for the nrp monitor, this monitor doesn't include a running state,
``sleepable`` and ``runnable`` are only referring to the task's desired
state, which could be scheduled out (e.g. due to preemption). However, it does
include the event ``sched_switch_in`` to represent when a task is allowed to
become running. This can be triggered also by preemption, but cannot occur
after the task got to ``sleeping`` until a ``wakeup``::

  sched_set_state_runnable
  sched_wakeup
  sched_switch_vain_preempt     |
  sched_switch_preempt          |
  sched_switch_yield            v
  sched_switch_vain        #=============================================#
       +-----------------> H                                             H
       |                   H                                             H
       +------------------ H                  runnable                   H
                           H                                             H
       +-----------------> H                                             H
       |                   #=============================================#
  sched_set_state_runnable   |                          |            ^
  sched_wakeup               |               sched_switch_blocking   |
       |          sched_set_state_sleepable             |            |
       |                     v                          |            |
       |                   +------------------------+   |       sched_wakeup
       +------------------ |                        |   |            |
                           |                        |   |            |
       +-----------------> |        sleepable       |   |            |
       |                   |                        |   |            |
       +------------------ |                        |   |            |
  sched_switch_in          +------------------------+   |            |
  sched_switch_preempt                  |               |            |
  sched_switch_vain_preempt    sched_switch_suspend     |            |
  sched_set_state_sleepable    sched_switch_blocking    |            |
                                        |               |            |
                                        v               |            |
                           +------------------------+   |            |
                           |         sleeping       | <-+            |
                           +------------------------+                |
                                        |                            |
                                        +----------------------------+

Monitor opid
------------

The operations with preemption and irq disabled (opid) monitor ensures
operations like ``wakeup`` and ``need_resched`` occur with interrupts and
preemption disabled or during IRQs, in such case preemption may not be disabled
explicitly.
``need_resched`` can be set by some RCU internals functions, in which case it
doesn't match a task wakeup and might occur with only interrupts disabled::

                 |                     sched_need_resched
                 |                     sched_waking
                 |                     irq_entry
                 |                   +--------------------+
                 v                   v                    |
               +------------------------------------------------------+
  +----------- |                     disabled                         | <+
  |            +------------------------------------------------------+  |
  |              |                 ^                                     |
  |              |          preempt_disable      sched_need_resched      |
  |       preempt_enable           |           +--------------------+    |
  |              v                 |           v                    |    |
  |            +------------------------------------------------------+  |
  |            |                   irq_disabled                       |  |
  |            +------------------------------------------------------+  |
  |                              |             |        ^                |
  |                          irq_entry         |        |                |
  |     sched_need_resched       v             |   irq_disable           |
  |     sched_waking +--------------+          |        |                |
  |           +----- |              |     irq_enable    |                |
  |           |      |    in_irq    |          |        |                |
  |           +----> |              |          |        |                |
  |                  +--------------+          |        |          irq_disable
  |                     |                      |        |                |
  | irq_enable          | irq_enable           |        |                |
  |                     v                      v        |                |
  |            #======================================================#  |
  |            H                     enabled                          H  |
  |            #======================================================#  |
  |              |                   ^         ^ preempt_enable     |    |
  |       preempt_disable     preempt_enable   +--------------------+    |
  |              v                   |                                   |
  |            +------------------+  |                                   |
  +----------> | preempt_disabled | -+                                   |
               +------------------+                                      |
                 |                                                       |
                 +-------------------------------------------------------+

References
----------

[1] - https://bristot.me/linux-task-model
