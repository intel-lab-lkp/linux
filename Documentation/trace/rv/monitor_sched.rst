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

References
----------

[1] - https://bristot.me/linux-task-model
