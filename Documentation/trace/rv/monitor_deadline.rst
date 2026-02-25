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
On systems with CPU frequency scaling or turbo boost, deadline tasks can run
longer than their runtime as this is scaled according to the frequency. In this
scenario, the monitor allows to skip the runtime check with the module
parameter ``throttle.skip_runtime_check``.

Servers can be also in the ``armed`` state, which represents when the
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
   |    |                            |         ^                |   |    |   |
   |    |                        dl_throttle  dl_replenish      |   |    |   |
   |    |                            v         |                |   |    |   |
   |    |     dl_defer_arm  +-------------------+               |   v    v   |
   |    |       +---------- |                   |             +--------------+
   |    |       |           |                   |             |              |
   |    |       +---------> |  armed_throttled  |             |  preempted   |
   |    |                   |                   |             |              |
   |    +-----------------> |                   |             +--------------+
   |          dl_defer_arm  +-------------------+   sched_switch_out ^   |  ^
   |                            |              ^        dl_replenish |   |  |
   |                  sched_switch_out    dl_defer_arm          +----+   |  |
   |                            v              |                         |  |
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
before their deadline, although deferrable servers may not run. An entity is
considered done if ``throttled``, either because it yielded or used up its
runtime, or when it voluntarily starts ``sleeping``.
The monitor includes a user configurable deadline threshold. If the total
utilisation of deadline tasks is larger than 1, they are only guaranteed
bounded tardiness. See Documentation/scheduler/sched-deadline.rst for more
details. The threshold (module parameter ``nomiss.deadline_thresh``) can be
configured to avoid the monitor to fail based on the acceptable tardiness in
the system. Since ``dl_throttle`` is a valid outcome for the entity to be done,
the minimum tardiness needs be 1 tick to consider the throttle delay, unless
the ``HRTICK_DL`` scheduler feature is active.

Servers have also an intermediate ``idle`` state, occurring as soon as no
runnable task is available from ready or running where no timing constraint
is applied. A server goes to sleep by stopping, there is no wakeup equivalent
as the order of a server starting and replenishing is not defined, hence a
server can run from sleeping without being ready::

                                  |
  sched_wakeup                    v
  dl_replenish;reset(clk) -- #=========================#
               |             H                         H dl_replenish;reset(clk)
               +-----------> H                         H <--------------------+
                             H                         H                      |
      +- dl_server_stop ---- H          ready          H                      |
      |  +-----------------> H   clk < DEADLINE_NS()   H   dl_throttle;       |
      |  |                   H                         H     is_defer == 1    |
      |  | sched_switch_in - H                         H -----------------+   |
      |  |   |               #=========================#                  |   |
      |  |   |                       |            ^                       |   |
      |  |   |             dl_server_idle    dl_replenish;reset(clk)      |   |
      |  |   |                       v            |                       |   |
      |  |   |                      +--------------+                      |   |
      |  |   |              +------ |              |                      |   |
      |  |   |     dl_server_idle   |              | dl_throttle          |   |
      |  |   |              |       |     idle     | -----------------+   |   |
      |  |   |              +-----> |              |                  |   |   |
      |  |   |                      |              |                  |   |   |
      |  |   |                      |              |                  |   |   |
   +--+--+---+--- dl_server_stop -- +--------------+                  |   |   |
   |  |  |   |                       |           ^                    |   |   |
   |  |  |   |            sched_switch_in    dl_server_idle           |   |   |
   |  |  |   |                       v           |                    |   |   |
   |  |  |   |      +---------- +---------------------+               |   |   |
   |  |  |   | sched_switch_in  |                     |               |   |   |
   |  |  |   | sched_wakeup     |                     |               |   |   |
   |  |  |   | dl_replenish;    |      running        | -------+      |   |   |
   |  |  |   |      reset(clk)  | clk < DEADLINE_NS() |        |      |   |   |
   |  |  |   |      +---------> |                     | dl_throttle   |   |   |
   |  |  |   +----------------> |                     |        |      |   |   |
   |  |  |                      +---------------------+        |      |   |   |
   |  | sched_wakeup                ^   sched_switch_suspend   |      |   |   |
   v  v dl_replenish;reset(clk)     |   dl_server_stop         |      |   |   |
 +--------------+                   |   |                      v      v   v   |
 |              | - sched_switch_in +   |                     +---------------+
 |              | <---------------------+     dl_throttle +-- |               |
 |   sleeping   |                            sched_wakeup |   |   throttled   |
 |              | -- dl_server_stop        dl_server_idle +-> |               |
 |              |    dl_server_idle     sched_switch_suspend  +---------------+
 +--------------+ <---------+                                        ^
        |                                                            |
        +------ dl_throttle;is_constr_dl == 1 || is_defer == 1 ------+

Monitor boost
~~~~~~~~~~~~~

The boost monitor ensures tasks associated to a server (e.g. fair tasks) run
either independently or boosted in a timely manner.
Unlike other models, the ``running`` state (and the ``switch_in/out`` events)
indicates that any fair task is running, this needs to happen within a
threshold that depends on server deadline and remaining runtime, whenever a
task is ready.

The following chart is simplified to avoid confusion, several less important
self-loops on states have been removed and event names have been simplified:

* ``idle`` (``dl_server_idle``) occurs when the CPU runs the idle task.
* ``start/stop`` (``dl_server_start/stop``) start and stop the server.
* ``switch`` (``sched_switch_in/out``) represented as a double arrow to
  indicate both edges are present: ``ready -- switch_in -> running`` and
  ``running -- switch_out -> ready``. As stated above this fires when any fair
  task starts or stops to running.
* ``resume/resume_throttle``: a fair task woke up, potentially when the server
  is throttled (no runtime left), this event is especially frequent on self
  loops (no state change during a wakeup) but is removed here for clarity.
* arrows merge with an ``x`` sign to indicate they are the same event going to
  the same state (but with different origins, e.g. ``{idle/throttled} -- stop
  -> stopped``). The ``+`` sign indicates standard crossings or corners.

Refer to the dot file for the full specification::

                      |
                      v
                #===============#        stop;reset(clk)
                H               H <---------------+
  +------------>H    stopped    H                 |
  |             H               H                 |
  |             #===============#                 |
  |                 ^          |                  |
  |                 |          |                  |      replenish;reset(clk)
  |               stop         |                  |                    +--+
  |                 |     start;reset(clk)        +-----------------+  |  |
  |                 |          v                                    |  |  v
  |                +---------------+ <---------- switch --------> +---------+
  |   +- resume -> |     ready     |                              |         |
  |   |            |               | -replenish;reset(clk)        | running |
  |   |  +- idle - | clk < thesh() |   |                          |         |
  |   |  |         +---------------+ <-+        +---------------- +---------+
  |   |  |         |  ^                         |                   ^    |
  |   |  |         |  |                       throttle              |    |
  |   |  |         |  |replenish;reset(clk)     |                   |    |
  |   |  |  throttle  |                         |   replenish;reset(clk) |
  |   |  |         |  |                         |                   |    |
  |   |  |         v  |                         v                   |    |
  |   |  |   +---------+    switch    +-------------------+         |    |
  x---+--+-- |         | <----------> | throttled_running | --------+    |
  |   |  |   |throttled|              +-------------------+              |
  |   |  |   |         | -----+            |                             |
  |   |  |   +---------+      |            |                             |
  |   |  |      ^             |            |                             |
  |   |  | resume_throttle    |            |                             |
 stop |  |      |             |            |                             |
  |   |  v      |             |            |                             |
  |   +---------+ <-----------x--- idle ---x-----------------------------+
  |   |         |
  +-- |  idle   | <--+
      |         |    | replenish;reset(clk)
      +---------+ ---+

Monitor laxity
~~~~~~~~~~~~~~

The laxity monitor ensure deferrable servers go to a zero-laxity wait unless
already running and run in starvation cases. The model can stay in the
zero-laxity wait only for up to a period, then the server either prepares to
stop (after ``idle_wait``) or prepares to boost a task (``running``). Boosting
(``sched_switch_in``) is only allowed in the ``running`` state.
``dl_replenish_running`` should not be allowed in ``running``, but can happen
as soon as the server started, the model allows this only within a short
threshold::

                                                  |
 +---- dl_server_stop -----+                      |
 |                         v                      v
 |            #=======================================#
 |   +------- H                stopped                H
 |   |        #=======================================#
 |   |          |                             ^
 |   |  dl_server_start_running;        dl_server_stop
 |   |        reset(clk)                      |
 |   |          v                             |            dl_replenish_running;
 |   |     +-------------------------------------+ -----------clk < REPLENISH_NS
 |   |     |                                     |              |
 |   |     |              running                | <------------+
 |   |     |                                     |
 |   |     +-------------------------------------+ ------------------+
 |   |       |            ^                    ^                     |
 |   |  dl_throttle    dl_replenish_running    |               dl_update
 |   |       v            |                    |        dl_replenish;reset(clk)
 |   |   +-------------------+                 |   dl_replenish_idle;reset(clk)
 |   |   |  replenish_wait   |                 |                     |
 |   |   | clk < period_ns() | ----------------+---------------------+--------+
 |   |   +-------------------+                 |                     |        |
 |   |                   |                     |                     |        |
 |   |               dl_update                 |                     |        |
 |   |         dl_replenish;reset(clk)     dl_replenish_running      |        |
 |   |                   v                     |                     |        |
 |   |                 +--------------------------+                  |        |
 | dl_server_start;    |                          | <----------------+        |
 |   reset(clk)        |     zero_laxity_wait     |                           |
 |   |                 |     clk < period_ns()    | ------+ dl_replenish;     |
 |   +---------------> |                          |       |    reset(clk)     |
 |                     +--------------------------+ <-----+ dl_update         |
 |                               |              ^                             |
 |  dl_replenish_idle;reset(clk) |      dl_replenish;reset(clk)               |
 |                               v            dl_update                       |
 |                  +------------------------+  |                             |
 +----------------- |        idle_wait       | -+                             |
                    |   clk < period_ns()    |                                |
                    +------------------------+ <-- dl_replenish_idle;reset(clk)
                         ^             |
                         +------dl_replenish_idle;reset(clk)
