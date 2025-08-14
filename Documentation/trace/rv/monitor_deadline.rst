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
when it's actively boosting a task, only there the runtime is enforced::

                                     |
                                     |
      dl_replenish;reset(clk)        v
              sched_switch_in   #=========================# sched_switch_in;
               +--------------- H                         H   reset(clk)
               |                H                         H <----------------+
               +--------------> H         running         H                  |
    dl_throttle                 H clk < runtime_left_ns() H                  |
  +---------------------------- H                         H sched_switch_out |
  |      +--------------------> H                         H -------------+   |
  |     dl_replenish;           #=========================#              |   |
  |      reset(clk)                  |             ^                     |   |
  |      |                    dl_defer_arm   sched_switch_in;            |   |
  |      |                           |         reset(clk)                |   |
  v      |                           v             |                     |   |
 +------------+       dl_replenish  +----------------+                   |   |
 |            |       dl_defer_arm  |                | sched_switch_out  |   |
 | throttled  |         +---------- |     armed      | -------------+    |   |
 |            |         |           |                | <--------+   |    |   |
 +------------+         +---------> |                | dl_defer_arm |    |   |
   |      |                         +----------------+          |   |    |   |
   |      |                             |         ^             |   |    |   |
   |      |                         dl_throttle  dl_replenish   |   |    |   |
   |      | dl_throttle;yielded==1      v         |             |   |    |   |
   |      |   dl_defer_arm         +--------------------+       |   v    v   |
   |      |            +---------- |                    |     +--------------+
   |      |            |           |                    |     |              |
   |      |            +---------> |  armed_throttled   |     |  preempted   |
   |      |                        |                    |     |              |
   |      +----------------------> |                    |     +--------------+
   |        dl_defer_arm           +--------------------+              ^
   |                                 |                ^                |
   |                         sched_switch_out         | dl_defer_arm   |
   |                                 v                |                |
   |             sched_switch_out  +-------------------------+         |
   |               +-------------- |                         |   dl_replenish
   |               |               |                         |         |
   |               +-------------> |   preempted_throttled   | --------+
   |                               |                         |
   +-----------------------------> |                         |
         sched_switch_out          +-------------------------+


Monitor nomiss
~~~~~~~~~~~~~~

The nomiss monitor ensures dl entities run to completion before their
deadiline. An entity is considered done if throttled, either because it yielded
or used up its runtime, or when it goes to sleep.
The monitor includes a user configurable deadline threshold. If the total
utilisation of deadline tasks is larger than 1, they are only guaranteed
bounded tardiness. See Documentation/scheduler/sched-deadline.rst for more
details. The threshold (module parameter ``nomiss.deadline_thresh``) can be
configured to avoid the monitor to fail based on the acceptable tardiness in
the system::

                             sched_switch_in
                             sched_wakeup
                           +----------------------+
                           v                      |
                         #==========================#  sched_switch_suspend
               --------> H                          H ----------------+
                         H                          H                 v
                         H                          H           +----------+
                         H                          H           | sleeping |
                         H         running          H           +----------+
                         H clk < DEADLINE_LEFT_NS() H  sched_wakeup;  |
                         H                          H  reset(clk)     |
                         H                          H <---------------+
     +-----------------> H                          H -+
     |                   #==========================#  |
     |                                                 |
     |                       sched_switch_suspend      |
 sched_switch_in             dl_throttle               |
 sched_wakeup;reset(clk)   +----------------------+    | dl_throttle
     |                     v                      |    |
     |                   +--------------------------+  |
     +------------------ |        throttled         | <+
                         +--------------------------+
