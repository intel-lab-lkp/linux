Monitor stall
=============

- Name: stall - wakeup in preemptive
- Type: per-task hybrid automaton
- Author: Gabriele Monaco <gmonaco@redhat.com>

Description
-----------

The stalled task (stall) monitor is a sample per-task timed monitor that checks
if tasks are scheduled within a defined threshold after they are ready::

                        |
                        |
                        v
                      #==================================#
                      H             dequeued             H <+
                      #==================================#  |
                        |                                   |
                        | sched_wakeup;reset(clk)           |
                        v                                   |
                      +----------------------------------+  |
                      |             enqueued             |  |
                      |     clk < threshold_jiffies      |  | sched_switch_wait
                      +----------------------------------+  |
                        |                                   |
                        | sched_switch_in                   |
    sched_switch_in     v                                   |
    sched_wakeup      +----------------------------------+  |
  +------------------ |                                  |  |
  |                   |             running              |  |
  +-----------------> |                                  | -+
                      +----------------------------------+


The threshold can be configured as a parameter by either booting with the
``stall.threshold_jiffies=<new value>`` argument or writing a new value to
``/sys/module/stall/parameters/threshold_jiffies``.

Specification
-------------
Grapviz Dot file in tools/verification/models/stall.dot
