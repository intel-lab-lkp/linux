Scheduler monitors
==================

- Name: deadline
- Type: container for multiple monitors
- Author: Gabriele Monaco <gmonaco@redhat.com>

Description
-----------

The deadline monitor is a set of specifications to describe the deadline
scheduler behaviour. It includes monitors per scheduling entity (deadline tasks
and servers) that work independently to verify different specifications the
deadline scheduler should follow.

Specifications
--------------

Monitor throttle
~~~~~~~~~~~~~~~~

The throttle monitor ensures deadline entities are throttled when they use up
their runtime. Deadline tasks can be only ``running``, ``preempted`` and
``throttled``, the runtime is enforced only in ``running`` based on an internal
clock and the runtime value in the deadline entity.

Servers can be also in the ``armed`` state, which corresponds to when the
server is consuming bandwidth in background (e.g. idle or normal tasks are
running without any boost). From this state the server can be throttled but it
can also use more runtime than available. A server is considered ``running``
when it's actively boosting a task, only there the runtime is enforced. The
server is preempted if the running task is not in the server's runqueue (e.g. a
FIFO task for the fair server).
Events like ``dl_armed`` and ``sched_switch_in`` can occur sequentially for
servers since they are related to the current task (e.g. a 2 fair tasks can be
switched in sequentially, that corresponds to multiple ``dl_armed``).

Any task or server in the ``throttled`` state must leave it shortly, e.g.
become ``preempted``::

                                     |
                                     |
      dl_replenish;reset(clk)        v
              sched_switch_in   #=========================# sched_switch_in;
               +--------------- H                         H   reset(clk)
               |                H                         H <----------------+
               +--------------> H         running         H                  |
    dl_throttle;reset(clk)      H clk < runtime_left_ns() H                  |
   +--------------------------- H                         H sched_switch_out |
   |       +------------------> H                         H -------------+   |
   | dl_replenish;reset(clk)    #=========================#              |   |
   |       |                         |             ^                     |   |
   v       |                  dl_defer_arm         |                     |   |
 +-------------------------+         |             |                     |   |
 |       throttled         |         |    sched_switch_in;reset(clk)     |   |
 | clk < THROTTLED_TIME_NS |         v             |                     |   |
 +-------------------------+        +----------------+                   |   |
   |    |                           |                | sched_switch_out  |   |
   |    |               +---------- |                | -------------+    |   |
   |    |          dl_replenish     |     armed      |              |    |   |
   |    |          dl_defer_arm     |                | <--------+   |    |   |
   |    |               +---------> |                | dl_defer_arm |    |   |
   |    |                           +----------------+          |   |    |   |
   |    |                               |         ^             |   |    |   |
   |    |                           dl_throttle  dl_replenish   |   |    |   |
   |    |   dl_throttle;yielded==1      v         |             |   |    |   |
   |    |        dl_defer_arm      +-------------------+        |   v    v   |
   |    |              +---------- |                   |      +--------------+
   |    |              |           |                   |      |              |
   |    |              +---------> |  armed_throttled  |      |  preempted   |
   |    |                          |                   |      |              |
   |    +------------------------> |                   |      +--------------+
   |        dl_defer_arm           +-------------------+                 |  ^
   |                                 |              ^                    |  |
   |                       sched_switch_out    dl_defer_arm              |  |
   |                                 v              |                    |  |
   |       sched_switch_out  +-----------------------+                   |  |
   |         +-------------- |                       | dl_throttle;      |  |
   |         |               |                       |  is_constr_dl==1  |  |
   |         +-------------> |  preempted_throttled  | <-----------------+  |
   |                         |                       |                      |
   +-----------------------> |                       | -- dl_replenish -----+
         sched_switch_out    +-----------------------+

The value of ``runtime_left_ns()`` is directly read from the deadline entity
and updated as the task runs. It is increased by 1 tick to account for the
maximum delay to throttle (not valid if ``sched_feat(HRTICK_DL)`` is active).

Monitor nomiss
~~~~~~~~~~~~~~

The nomiss monitor ensures dl entities get to run *and* run to completion
before their deadiline, although deferrable servers may not run. An entity is
considered done if ``throttled``, either because it yielded or used up its
runtime, or when it voluntarily starts ``sleeping``.
The monitor includes a user configurable deadline threshold. If the total
utilisation of deadline tasks is larger than 1, they are only guaranteed
bounded tardiness. See Documentation/scheduler/sched-deadline.rst for more
details. The threshold (module parameter ``nomiss.deadline_thresh``) can be
configured to avoid the monitor to fail based on the acceptable tardiness in
the system. Since ``dl_throttle`` is a valid outcome for the entity to be done,
the minimum tardiness needs be 1 tick to consider the throttle delay.

Servers have also an intermediate ``idle`` state, occurring as soon as no
runnable task is available. When a server goes to ``sleeping`` it is guaranteed
to be done for the period (unlike tasks), hence it cannot be considered
``ready`` before its runtime is replenished::

              sched_wakeup        |
           dl_server_start        |
   dl_replenish;reset(clk)        v
               +------------ #=========================#
               |             H                         H dl_replenish;reset(clk)
               +-----------> H                         H <------------------+
                             H                         H                    |
 +- dl_server_stop --------- H          ready          H                    |
 |                           H   clk < DEADLINE_NS()   H                    |
 |    +--------------------> H                         H   dl_throttle;     |
 |    |                      H                         H   is_defer == 1    |
 |    |   sched_switch_in -- H                         H ---------------+   |
 |    |    |                 #=========================#                |   |
 |    |    |                       |             ^                      |   |
 |    |    |             dl_server_idle     dl_replenish;reset(clk)     |   |
 |    |    |                       v             |                      |   |
 |    |    |                      +----------------+                    |   |
 |    |    |          +---------- |                |                    |   |
 |    |    |       dl_server_idle |      idle      | dl_throttle        |   |
 |    |    |          +---------> |                | ---------------+   |   |
 |    |    |                      +----------------+                |   |   |
 |    |    |                       |             ^                  |   |   |
 |    |    |            sched_switch_in      dl_server_idle         |   |   |
 |    |    |                       v             |                  |   |   |
 |    |    |        +---------- +---------------------+             |   |   |
 |    |    |   sched_switch_in  |                     |             |   |   |
 |    |    |   sched_wakeup     |                     | dl_throttle |   |   |
 |    |    |   dl_replenish;    |      running        | -------+    |   |   |
 |    |    |        reset(clk)  | clk < DEADLINE_NS() |        |    |   |   |
 |    |    |        +---------> |                     |        |    |   |   |
 |    |    +------------------> |                     |        |    |   |   |
 |    |                         +---------------------+        |    |   |   |
 |  sched_wakeup                   | sched_switch_suspend      |    |   |   |
 v  dl_replenish;reset(clk)        | dl_server_stop            |    |   |   |
 +---------------+                 |                           v    v   v   |
 |               | <---------------+          dl_throttle     +--------------+
 |               |                           sched_wakeup +-- |              |
 |   sleeping    | --+                     dl_server_idle |   |   throttled  |
 |               |  dl_server_start       dl_server_start +-> |              |
 |               |  dl_server_idle      sched_switch_suspend  +--------------+
 +---------------+ <-+                                                ^
         |                 dl_throttle;is_constr_dl == 1              |
         +------------------------------------------------------------+
