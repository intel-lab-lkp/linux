==========================
Real-Time group scheduling
==========================

.. CONTENTS

   0. WARNING
   1. Overview
     1.1 The problem
     1.2 The solution
   2. The interface
     2.1 System-wide settings
     2.2 Default behaviour
     2.3 Basis for grouping tasks
   3. Future plans


0. WARNING
==========

 Fiddling with these settings can result in an unstable system, the knobs are
 root only and assumes root knows what he is doing.

Most notable:

 * very small values in sched_rt_period_us can result in an unstable
   system when the period is smaller than either the available hrtimer
   resolution, or the time it takes to handle the budget refresh itself.

 * very small values in sched_rt_runtime_us can result in an unstable
   system when the runtime is so small the system has difficulty making
   forward progress (NOTE: the migration thread and kstopmachine both
   are real-time processes).

1. Overview
===========


1.1 The problem
---------------

Real-time scheduling is all about determinism, a group has to be able to rely on
the amount of bandwidth (eg. CPU time) being constant. In order to schedule
multiple groups of real-time tasks, each group must be assigned a fixed portion
of the CPU time available.  Without a minimum guarantee a real-time group can
obviously fall short. A fuzzy upper limit is of no use since it cannot be
relied upon. Which leaves us with just the single fixed portion.

1.2 The solution
----------------

CPU time is divided by means of specifying how much time can be spent running
in a given period. We allocate this "run time" for each real-time group which
the other real-time groups will not be permitted to use.

Each real-time group runs at the same priority as SCHED_DEADLINE, thus they
share and contend the SCHED_DEADLINE allowed bandwidth. Any time not allocated
to a real-time group (and SCHED_DEADLINE tasks) will be used to run both
SCHED_FIFO/SCHED_RR, normal priority tasks (SCHED_OTHER), and SCHED_EXT tasks,
following the usual priorities. Any allocated run time not used will also be
picked up by the other scheduling classes, in the same order as before.

Let's consider an example: a frame fixed real-time renderer must deliver 25
frames a second, which yields a period of 0.04s per frame. Now say it will also
have to play some music and respond to input, leaving it with around 80% CPU
time dedicated for the graphics. We can then give this group a run time of 0.8
* 0.04s = 0.032s.

This way the graphics group will have a 0.04s period with a 0.032s run time
limit. Now if the audio thread needs to refill the DMA buffer every 0.005s, but
needs only about 3% CPU time to do so, it can do with a 0.03 * 0.005s =
0.00015s. So this group can be scheduled with a period of 0.005s and a run time
of 0.00015s.

The remaining CPU time will be used for user input and other tasks. Because
real-time tasks have explicitly allocated the CPU time they need to perform
their tasks, buffer underruns in the graphics or audio can be eliminated.

2. The Interface
================


2.1 System wide settings
------------------------

The system wide settings are configured under the /proc virtual file system:

``/proc/sys/kernel/sched_rt_period_us``:
  The scheduling period that is equivalent to 100% CPU bandwidth.

``/proc/sys/kernel/sched_rt_runtime_us``:
  A global limit on how much time real-time scheduling may use (SCHED_DEADLINE
  tasks + real-time groups). This is always less or equal to the period_us, as
  it denotes the time allocated from the period_us for the real-time tasks.
  Without **CONFIG_RT_GROUP_SCHED** enabled, this only serves for admission
  control of deadline tasks. With **CONFIG_RT_GROUP_SCHED=y** it also signifies
  the total bandwidth available to both real-time groups and deadline tasks.

  * Time is specified in us because the interface is s32. This gives an
    operating range from 1us to about 35 minutes.
  * ``sched_rt_period_us`` takes values from 1 to INT_MAX.
  * ``sched_rt_runtime_us`` takes values from -1 to ``sched_rt_period_us``.
  * A run time of -1 specifies runtime == period, ie. no limit.
  * ``sched_rt_runtime_us/sched_rt_period_us`` > 0.05 inorder to preserve
    bandwidth for fair dl_server. For accurate value check average of
    runtime/period in ``/sys/kernel/debug/sched/fair_server/cpuX/``

The default value for ``sched_rt_period_us`` is 1000000 (or 1s) and for
``sched_rt_runtime_us`` is 950000 (or 0.95s). This gives a minimum of 0.05s to
be used by SCHED_FIFO/SCHED_RR and non-RT tasks (SCHED_OTHER, SCHED_EXT), while
0.95s are the maximum to be used by SCHED_DEADLINE, and rt-cgroups if enabled.

2.2 Cgroup settings
-------------------

Enabling **CONFIG_RT_GROUP_SCHED** lets you explicitly allocate real CPU
bandwidth to task groups.

This uses the cgroup virtual file system and the CPU controller for cgroups.
Enabling the controller for the hierarchy creates two files:

* ``<cgroup>/cpu.rt_period_us``, the scheduling period of the group.
* ``<cgroup>/cpu.rt_runtime_us``, the maximum runtime each CPU will provide
  every period.

 .. tip::
  For more information on working with control groups, you should read
  *Documentation/admin-guide/cgroup-v1/cgroups.rst* as well.
 ..

By default the root cgroup has the same period of
``/proc/sys/kernel/sched_rt_period_us``, which is 1s, and a runtime of zero, so
that rt-cgroup is *soft-disabled* by default, and all the runtime is available
for SCHED_DEADLINE tasks only. New groups instead get both period and runtime of
zero.

2.3 Cgroup Hierarchy and Behaviours
-----------------------------------

With HCBS, cgroups may act either as task runners or bandwidth reservation:

* A bandwidth reservation cgroup (such as the root control group), has the
  purpose to reserve a portion of the total real-time bandwidth for its sub-tree
  of groups. A group in this state cannot run SCHED_FIFO/SCHED_RR tasks.

  .. important::
    The *root control group* behaviour is different from the other cgroups, as
    its job is to reserve bandwidth for the whole group hierarchy, but it can
    also run rt tasks. This is an exception: FIFO/RR tasks running in the
    root cgroup follow the same rules as FIFO/RR tasks in a kernel which has
    **CONFIG_RT_GROUP_SCHED=n**, and the bandwidth reservation is instead a
    feature connected to HCBS, that acts on the cgroup tree.
  ..

* A *live* group instead can be used to run FIFO/RR tasks, with the given
  bandwidth parameters: each CPU is served a *potentially continuous* runtime of
  ``<cgroup>/cpu.rt_runtime_us`` every period ``<cgroup>/cpu.rt_period_us``. It
  is important to notice that increasing the period but leaving the bandwidth
  constant changes the behaviour of the cgroup's servers, as the bandwidth given
  overall is the same, but it is given in longer bursts (and longer slices of no
  bandwidth).

More specifically on *live* and non-*live*:

* A group is deemed *live* if it is a leaf of the groups' hierarchy or all of
  its children have runtime 0.
* *Live* groups are the only groups allowed to run real-time tasks. A SCHED_FIFO
  task cannot be migrated in a non-*live* group, neither a task inside this
  group can change scheduling policy to SCHED_FIFO/SCHED_RR if the group is not
  *live*.
* Non-*live* groups are only used for bandwidth reservation.
* Group's bandwidth follow this invariant: the sum of the bandwidths of a
  group's children is always less than or equal to the group's bandwidth.

Real-time group scheduling means you have to assign a portion of total CPU
bandwidth to the group before it will accept real-time tasks. Therefore you will
not be able to run real-time tasks as any user other than root until you have
done that, even if the user has the rights to run processes with real-time
priority!


3. Theoretical Background
=========================


 ..  BIG FAT WARNING ******************************************************

 .. warning::

   This section contains a (not-thorough) summary on deadline/hierarchical
   scheduling theory, and how it applies to real-time control groups.
   The reader can "safely" skip to Section 4 if only interested in seeing
   how the scheduling policy can be used. Anyway, we strongly recommend
   to come back here and continue reading (once the urge for testing is
   satisfied :P) to be sure of fully understanding all technical details.

 .. ************************************************************************

The real-time cgroup scheduler is based upon the **Hierarchical Constant
Bandwidth Server** (HCBS) [1] *Compositional Scheduling Framework* (CSF). A
**CSF** is a framework where global (system-level) timing properties can be
established by composing independently (specified and) analyzed local
(component-level) timing properties [5].

For HCBS (related to the Linux kernel), the compositional framework consists of
two parts:

* The *scheduling components*, which are the basic units of the scheduling. In
  the kernel these are the single cgroups along with the tasks that must be run
  inside.

* The *scheduling resources*, which are the CPUs of the machine.

HCBS is a *hierarchical scheduling framework*, where the scheduling components
form a hierarchy and resources are allocated from parent components to its child
components in the hierarchy.

The Chapter is organized as follows: **Section 3.1** gives basic real-time
theory definitions that are used throughout the whole section. **Section 3.2**
talks about the HCBS framework, giving a general idea on how this is structured.
**Section 3.3** introduces the MPR model, one of the many models which may be
used for the analysis of the scheduling components and the computation of the
minimum required scheduling resources for a given component. **Section 3.4**
shows the schedulability test for MPR on the HCBS framework. **Section 3.5**
shows how to convert a MPR interface to a HCBS compatible resource reservation
for a component. Finally, **Section 3.6** lists other interesting models which
could be used for the component analysis in HCBS.

3.1 Basic Definitions
---------------------

*We borrow the same definitions given in the* ``sched_deadline`` *document, which
are very briefly summarized here, and new ones, needed by the following content,
are added.*

A typical real-time task is composed of a repetition of computation phases (task
instances, or jobs) which are activated on a periodic or sporadic fashion. For
our purposes, real-time tasks are characterized by three parameters:

* Worst Case Execution Time (WCET): the maximum execution time among all jobs.
* Relative Deadline (D): the maximum time each job must be completed, relative
  to the release time of the job.
* Inter-Arrival Period (P): the exact/minimum (for periodic/sporadic tasks) time
  between each consecutive job.

3.2 Hierarchical Constant Bandwidth Server (HCBS) [1]
-----------------------------------------------------

As mentioned, HCBS is a *hierarchical scheduling framework*:

* The framework hierarchy follows the same hierarchy of cgroups. Cgroups may
  have two roles, either bandwidth reservation for children cgroups, or they may
  be *live*, i.e. run tasks (but not both). The root cgroup, for the kernel's
  implementation of HCBS, acts only as bandwidth reservation (but as written in
  this document it has also different uses outside of the hierarchical
  framework).
* The cgroup tree is internally flattened, for ease of scheduling, to a
  two-level hierarchy, since only the *live* groups are of interest and all the
  necessary information for their scheduling lies in their interface (there is
  no need for the reservation components).
* The hierarchical framework, now on two levels, consists then of a first level
  of cgroups, and a second level of tasks that are run inside these groups.
* The scheduling of components is performed using global Earliest Deadline First
  (gEDF), SCHED_DEADLINE in the kernel, following the bandwidth reservation of
  each group.
* Whenever a component is scheduled, a local scheduler picks which of the tasks
  of the cgroup to run. The scheduling policy is global Fixed Priority (gFP),
  SCHED_FIFO/SCHED_RR in the kernel.

3.3 Multiprocessor Periodic Resource (MPR) model
------------------------------------------------

A Multiprocessor Periodic Resource (MPR) model [2] **u = <Pi, Theta, m'>**
specifies that an identical, unit-capacity multiprocessor platform collectively
provides **Theta** units of resource every **Pi** time units, where the
**Theta** time units are supplied with concurrency at most **m'**.

This theoretical model is one of the many models that can abstract the
interface of our real-time cgroups: let **m'** be the number of CPUs of the
machine, let **Theta** be **m' * <cgroup>/cpu.rt_runtime_us** and **Pi** be
**<cgroup>/cpu.rt_period_us**.

Let's introduce the concept of Supply Bound Function (SBF). A SBF is a function
which outputs a lower bound for the processor supply provided in a given time
interval, given a resource supply model. For a completely dedicated CPU, the SBF
function is simply the identity function, as it will always provide **t** units
of computation for an interval of length **t**. The situation gets slightly more
complicated for the MPR model or any of the other model listed in section 3.6.

The **SBF(t)** for a MPR model **u = <Pi, Theta, m'>** is::

             | 0                                       if t' < 0
             |
  SBF_u(t) = | floor(t' / PI) * Theta
             |   + max(0, m' * x - (m' * Pi - Theta)   if t' >= 0 and 1 <= x <= y
             |
             | floor(t' / PI) * Theta
             |   + max(0, m' * x - (m' * Pi - Theta)   else
             |   - (m' - beta)

where::

  alpha = floor(Theta / m')
  beta = Theta - m' * alpha
  t' = t - (Pi - ceil(Theta / m'))
  x  = t' - (Pi * floor(t' / Pi))
  y  = Pi - floor(Theta / m')

Briefly, this function models that the server's bandwidth is given as late as
possible, so describing the worst case possible for the supplied bandwidth.

3.4 Schedulability for MPR on global Fixed-Priority
---------------------------------------------------

Let's introduce the concept of Demand Bound Function (DBF). A DBF is a function
that, given a taskset, a scheduling algorithm and an interval of time, outputs
the worst resource demand for that interval of time.

It is easy to see that, given a DBF and a SBF, we can deem a component/taskset
schedulable if, for every time interval t >= 0, it is possible to demonstrate
that:

  DBF(t) <= SBF(t)

We have the Supply Bound Function for our given MPR model, so we are missing the
Demand Bound Function for a given taskset that is being scheduled using global
Fixed Priority.

3.4.1 Schedulability Analysis for global Fixed Priority
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Bertogna, Cirinei and Lipari [6] have derived a schedulability test for global
Fixed Priority (gFP) on multi-processor platforms. In this test (called
*BCL_gFP* test) we can consider all the CPUs to be dedicated to the scheduling.

  A taskset **Tau** is schedulable with gFP on a multiprocessor platform
  composed of **m'** identical processors if for each task **tau_k in Tau**:

    Sum(for i < k)( min(W_i(D_k), D_k - C_k + 1) ) < m' * (D_k - C_k + 1)

  where **W_i(t)** is the workload of task **tau_i** over a time interval **t**:

    W_i(t) = N_i(t) * C_i + min(C_i, t + D_i - C_i - N_i(t) * P_i)

  and **N_i(t)** is the number of activations of task **tau_i** that complete in
  a time interval **t**:

    N_i(t) = floor( (t + D_i - C_i) / P_i )

  while the **min** term is the contribution of the carried-out job in the
  interval **t**, i.e. that job that does not completely fit in the interval
  **t**, but starts inside the interval after all the jobs that complete.

3.4.2 From BCL_gFP to the Demand Bound Function
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

We can then derive the DBF from this test:

  DBF_gFP(tau_k) = Sum(for i < k)( min(W_i(D_k), D_k - C_k + 1) ) + m' * (C_k - 1)

Briefly, the first sum component, the same in the BCL_gFP test, describes the
maximum interference that higher priority task give to the analysed task. The
workload is upperbounded by ``(D_k - C_K + 1)`` because we are only interested
in the interference in the slack time, while for the ``C_k`` time we are
requiring that all the CPUs are fully available, as the single job needs `C_k`
(non overlapping) time units to run.

The demand bound function from Bertogna et al. is only defined on a single time
(i.e. the deadline of the task in analysis) instead of all possible times as
this is the minimum argument to demonstrate schedulability on global Fixed
Priority.

3.4.3 Putting it all togheter
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A component **C**, on **m'** processors, running a taskset **Tau = { tau_1 =
(C_1, D_1, P_1), ..., tau_n = (C_n, D_n, P_n) }** of **n** sporadic tasks, is
schedulable under gFP using an MPR model **u = <Pi, Theta, m'>**, if for all
tasks **tau_k in Tau**:

  DBF_gFP(tau_k) <= SBF_u(D_K)

3.5 From MPR to deadline servers
--------------------------------

Since there exist no algorithm to schedule MPR interfaces, a tecnique was
developed to transform MPR interfaces into periodic tasks, so that a
number of periodic servers which respect the tasks requirements can be used for
the scheduling of the MPR interface and associated tasks.

Let **u = <Pi, Theta, m>** be a MPR interface, let **a = Theta - m * floor(Theta
/ m)**, let **k = floor(a)**. Define a transformation from **u** to a periodic
taskset **Tau_u = { tau_1 = (C_1, D_1, P_1), ..., tau_m' = (C_m', D_m', P_m')
}**, where:

  **tau_1 = ... = tau_k = (floor(Theta / m') + 1, Pi, Pi)**

  **tau_k+1 = (floor(Theta / m') + a - k * floor(a/k), Pi, Pi)**

  **tau_k+2 = ... = tau_m' = (floor(Theta / m'), Pi, Pi)**

This periodic taskset of servers **Tau_u** can be scheduled on any number of
processors with concurrency at most **m'**.

For real-time control groups, it is possible to just consider a slightly more
demanding taskset **Tau_u'**, where each task **tau_i** is defined as follows:

  **tau_i = (ceil(Theta / m'), Pi, Pi)**

3.6 Other models
----------------

There exist many other theoretical models in literature which are used to
describe a hierarchical scheduling framework on multi-core architectures.
Notable examples are the Multi Supply Function (MSF) abstraction [3], the
Parallel Supply Function (PSF) abstraction [4] and the Bounded Delay
Multipartition (BDM) [7].

3.7 References
--------------
  1 - L. Abeni, A. Balsini, and T. Cucinotta, “Container-based real-time
      scheduling in the Linux kernel,” SIGBED Rev., vol. 16, no. 3, pp. 33-38,
      Nov. 2019, doi: 10.1145/3373400.3373405.
  2 - A. Easwaran, I. Shin, and I. Lee, “Optimal virtual cluster-based
      multiprocessor scheduling,” Real-Time Syst, vol. 43, no. 1, pp. 25-59,
      Sept. 2009, doi: 10.1007/s11241-009-9073-x.
  3 - E. Bini, G. Buttazzo, and M. Bertogna, “The Multi Supply Function
      Abstraction for Multiprocessors,” in 2009 15th IEEE International
      Conference on Embedded and Real-Time Computing Systems and Applications,
      Aug. 2009, pp. 294-302. doi: 10.1109/RTCSA.2009.39.
  4 - E. Bini, B. Marko, and S. K. Baruah, “The Parallel Supply Function
      Abstraction for a Virtual Multiprocessor,” in Scheduling, S. Albers, S. K.
      Baruah, R. H. Möhring, and K. Pruhs, Eds., in Dagstuhl Seminar Proceedings
      (DagSemProc), vol. 10071. Dagstuhl, Germany: Schloss Dagstuhl -
      Leibniz-Zentrum für Informatik, 2010, pp. 1-14. doi:
      10.4230/DagSemProc.10071.14.
  5 - I. Shin and I. Lee, “Compositional real-time scheduling framework,” in
      25th IEEE International Real-Time Systems Symposium, Dec. 2004, pp. 57-67.
      doi: 10.1109/REAL.2004.15.
  6 - M. Bertogna, M. Cirinei, and G. Lipari, “Schedulability Analysis of Global
      Scheduling Algorithms on Multiprocessor Platforms,” IEEE Transactions on
      Parallel and Distributed Systems, vol. 20, no. 4, pp. 553-566, Apr. 2009,
      doi: 10.1109/TPDS.2008.129.
  7 - G. Lipari and E. Bini, “A Framework for Hierarchical Scheduling on
      Multiprocessors: From Application Requirements to Run-Time Allocation,” in
      2010 31st IEEE Real-Time Systems Symposium, Nov. 2010, pp. 249-258. doi:
      10.1109/RTSS.2010.12.


4. Using Real-Time cgroups
==========================

4.1 CGroup Setup
----------------

The following is a brief guide to the use of Real-Time Control Groups.

Of course, real-time control groups require mounting of the cgroup file system.
We have decided to only support cgroups v2, so make sure you mount the v2
controller for the cgroup hierarchy.

Additionally the real-time cgroups require the CPU controller for the cgroups to
be enabled::

  # Assume the cgroup file system is mounted at /sys/fs/cgroup
  > echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control

The CPU controller can only be mounted if there is no SCHED_FIFO/SCHED_RR task
scheduled in any cgroup other than the root control group.

The root control group has no bandwidth allocated by default, so make sure to
allocate some bandwidth so that it can be used by the other cgroups. More on
that in the following section...

4.2 Bandwidth Allocation for groups
-----------------------------------

Allocating bandwidth to a cgroup is a fundamental step to run real-time
workload. The cgroup filesystem exposes two files:

* ``<cgroup>/cpu.rt_runtime_us``: which specifies the cgroups' runtime in
  microseconds.
* ``<cgroup>/cpu.rt_period_us``: which specifies the cgroups' period in
  microseconds.

Both files are readable and writable, and their default value is zero. By
definition, the specified runtime must be always less than or equal to the
period. Additionally, an admission test checks if the bandwidth invariant is
respected (i.e. sum of children's bandwidth <= parent's bandwidth).

The root control group files instead control and reserve the SCHED_DEADLINE
bandwidth allocated to real-time cgroups, since real-time groups compete and
share the same bandwidth allocated to SCHED_DEADLINE tasks.

4.3 Running real-time tasks in groups
-------------------------------------

To run tasks in real-time groups it is just necessary to change a tasks
scheduling policy to SCHED_FIFO/SCHED_RR and migrate it into the group. If the
group is not allowed to run real-time tasks because of incorrect configuration,
either migrating a SCHED_FIFO/SCHED_RR task into the group or changing
scheduling policy to a task already inside the group will fail::

 # assume there is a task of PID 42 running
 # change its scheduling policy to SCHED_FIFO, priority 99
 > chrt -f -p 99 42

 # migrate the task to a cgroup
 > echo 42 > /sys/fs/cgroup/<my-cgroup>/cgroup.procs

4.4 Special case: the root control group
----------------------------------------

The root cgroup is special, compared to the other cgroups, as its tasks are not
managed by the HCBS algorithm, rather they just use the original
SCHED_FIFO/SCHED_RR policies (as if CONFIG_RT_GROUP_SCHED was disabled). As
mentioned, its bandwidth files are just used to control how much of the
SCHED_DEADLINE bandwidth is allocated to cgroups.

4.5 Guarantees and Special Behaviours
-------------------------------------

Real-time cgroups are run at the same priority level of SCHED_DEADLINE tasks.
Since this is the highest priority scheduling policy, and since the Constant
Bandwidth Server (CBS) enforces that the specified bandwidth requirements for
both groups and tasks cannot be overrun, real-time groups have the same
guarantees that SCHED_DEADLINE tasks have, i.e. they will be necessarily
supplied by the amount of bandwidth requested (whenever the admission tests
pass).

This means that, since SCHED_FIFO/SCHED_RR tasks (scheduled in the root control
group) are not subject to bandwidth controls, they are run at a lower priority
than the cgroups' counterparts. Nonetheless, a minimum amount of bandwidth, if
reserved, will always be available to run SCHED_FIFO/SCHED_RR workloads in the
root cgroup, while they will be able to use more runtime if any of the
SCHED_DEADLINE tasks or servers use less than their specified amount of
bandwidth. SCHED_OTHER tasks are instead scheduled as normal, at lower priority
than real-time workloads.

The aforementioned behaviour differs from the preceding RT_GROUP_SCHED
implementation, but this is necessary to give actual guarantees to the amount of
bandwidth given to rt-cgroups.