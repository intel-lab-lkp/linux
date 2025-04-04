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
happen only in thread context::


                        |
                        |
                        v
    sched_set_state   +------------------+
  +------------------ |                  |
  |                   |  thread_context  |
  +-----------------> |                  | <+
                      +------------------+  |
                        |                   |
                        | schedule_entry    | schedule_exit
                        v                   |
                                            |
                       scheduling_context  -+

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

Monitor srs
-----------

The switch after resched or sleep (srs) monitor describes conditions for
different types of task switch. This is a complex model, below we are going to
explain it step by step. Unfortunately splitting this into smaller monitor is
not trivial due to some shared events such as ``switch_in``::

                                                      set_runnable
                                  |                        wakeup +---+
                                  |                   switch_vain |   |
                                  v                               |   v      wakeup
                         #================================================#  set_runnable
      switch_in          H                                                H <----------+
    +------------------> H                    running                     H            |
    |                    H                                                H -----+     |
    |                    #================================================#      |     |
    |                         |        |                |         ^    ^         |     |
    |                         |  switch_yield      need_resched   |    |         |     |
    |                         |        |      need_resched_lazy   |    |         |     |
    |                set_sleepable     v                |         |    |         |     |
    |                         |      +-------------+    |         |    |         |     |
    |                +--------+----> |  preempted  | ---+- switch_in   |         |     |
    |                |        |      +-------------+    |              |         |     |
    |        switch_preempt   |        |                |              |         |     |
    |        switch_yield     |   need_resched          |   +- switch_vain       |     |
    |                |        |        v                |   |                    |     |
    |                |        |      +-------------+    |   |                    |     |
    |  need_resched -+--------+----> | resched_out |    |   |                    |     |
    |  |             |        |      +-------------+    |   |                    |     |
    |  |             |        |        |                |   |     need_resched   |     |
    |  |             |        |    switch_in            |   |     wakeup         |     |
    |  |             |        |        v                v   |     set_runnable   |     |
    |  |             |        |      +--------------------------+ -------+       |     |
    |  |             |        |      |                          |        |       |     |
    |  |             +--------+----- |       rescheduling       | <------+       |     |
    |  |                      |      |                          |                |     |
    |  |                      |      +--------------------------+ -----------+   |     |
    |  |                      |        |           ^ wakeup                  |   |     |
    |  |                      |  set_sleepable   set_runnable                |   |     |
    |  |                      |        v           |                         |   |     |
    |  |   +------------------+----- +---------------------------+           |   |     |
    |  |   |                  |      |                           |           |   |     |
 +--+--+---+------------------+----> |     resched_sleepable     | ---+      |   |     |
 |  |  |   |                  |      |                           |    |      |   |     |
 |  |  |   |    +-------------+----> +---------------------------+    |      |   |     |
 |  |  |   |    |             |        |           ^      |           |      |   |     |
 |  |  |   |    |             |  switch_preempt    | need_resched     |      |   |     |
 |  |  |   |    |             |        |           | set_sleepable    |      |   |     |
 |  |  |   |    |             |        v           +------+           |      |   |     |
 |  |  |   |    |             |       +---------------------------+ --+------+---+-----+--+
 |  |  |   |    |             |       |    preempted_sleepable    |   |      |   |     |  |
 |  |  |   |    |             |       +---------------------------+ --+------+---+--+  |  |
 |  |  |   |    |             |         |             ^               |      |   |  |  |  |
 |  |  |   |    |             |     switch_in   switch_preempt        |      |   |  |  |  |
 |  |  |   |    |             |         v             |          switch_vain |   |  |  |  |
 |  |  |   |    |             |        +-------------------------+    |      |   |  |  |  |
 |  |  |   |    |             +------> |                         | <--+      |   |  |  |  |
 |  |  |   |    |                      |        sleepable        |           |   |  |  |  |
 |  |  |   |    +- need_resched------- |                         | ----------+---+--+--+  |
 |  |  |   |       need_resched_lazy   +-------------------------+           |   |  |     |
 |  |  |   |                              |      ^      |          switch_block  |  |     |
 |  |  |   |                              |      | set_sleepable             |   |  |     |
 |  |  |   |                      switch_block   | switch_vain    +----------+   |  |     |
 |  |  |   |                    switch_suspend   +------+         |              |  |     |
 |  |  |   |                              v                       v              |  |     |
 |  |  |   |   switch_block          +-----------------------------+  switch_block  |     |
 |  |  |   +-switch_suspend--------> |          sleeping           | <-----------+  |     |
 |  |  |                             +-----------------------------+                |     |
 |  |  |                               | wakeup                                     |     |
 |  |  |                               v                                            |     |
 |  |  +- need_resched ------------- +-------------+  wakeup                        |     |
 |  |                                |   waking    | <------------------------------+     |
 |  +------------------------------- +-------------+                                      |
 |                                                                                        |
 |                         +-----------------------+                                      |
 +----- switch_in -------- | resched_out_sleepable | <-- sched_need_resched --------------+
                           +-----------------------+

Types of switches:

* ``switch_in``:
  a non running task is scheduled in, this leads to ``running`` if the task is
  runnable and ``sleepable`` if the task was preempted before sleeping.
* ``switch_suspend``:
  a task puts itself to sleep, this can happen only after explicitly setting
  the task to ``sleepable``. After a task is suspended, it needs to be woken up
  (``waking`` state) before being switched in again. The task can be set to
  ``resched_sleepable`` via a ``need_resched`` but not preempted, in which case it
  is equivalent to ``sleepable``.
  Setting the task's state to ``sleepable`` can be reverted before switching if it
  is woken up or set to runnable.
* ``switch_blocked``:
  a special case of a ``switch_suspend`` where the task is waiting on a
  sleeping RT lock (``PREEMPT_RT`` only), it is common to see wakeup and set
  state events racing with each other and this leads the model to perceive this
  type of switch when the task is not set to sleepable. This is a limitation of
  the model in SMP system and workarounds may require to slow down the
  scheduler.
* ``switch_yield``:
  a task explicitly calls the scheduler, this looks like a preemption as the
  task is still runnable but the ``need_resched`` flag is not set. It can
  happen after a ``yield`` system call or from the idle task.
* ``switch_preempt``:
  a task is ``preempted``, this can happen after the need for ``rescheduling``
  has been set, also in its ``lazy`` flavour. ``need_resched`` can be set as a
  flag to the task or in the per-core preemption count, either of them can
  trigger a preemption.
  The task was previously running and can be switched in directly, but it is
  possible that a task is preempted after it sets itself as ``sleepable``
  (``preempted_sleepable``), in this condition, once the task is switched back
  in, it will not be ``running`` but continue its sleeping process in
  ``sleepable``.
* ``switch_vain``:
  a task goes through the scheduler but it is picked as the next task to run,
  hence no real task switch occurs. Since we run the scheduler, this clears the
  need to reschedule.

The ``resched_out`` state (``resched_out_sleepable`` if the task is sleepable)
is a special case reached if the ``need_resched`` is set after picking the next
task but before switching. In this case, the previous task is switched out
normally but once it switches in, it can be preempted in ``rescheduling``. This
can happen, for instance, when a task disables migration and we do a
dequeue/enqueue before actually switching out.

This monitor has several events that can race with each other, for instance if
running from multiple CPUs (e.g. ``need_resched`` and ``wakeup`` can occur from
a remote CPU), we need to account for them in any possible order.

This monitor allows set state (runnable or sleepable) only in states
``running``, ``rescheduling``, ``resched_sleepable``, and ``sleepable``, and
not for states where the task is ``sleeping`` or ``preempted``. This implies
the set state event occurs only in the task's context::

                        |
                        |
                        v
                      +------------------+
                      |  other_context   | <+
                      +------------------+  |
                        |                   |
                        | switch_in         | switch_out
                        v                   |
    sched_set_state                         |
  +------------------                       |
  |                       own_context       |
  +----------------->                      -+

References
----------

[1] - https://bristot.me/linux-task-model
