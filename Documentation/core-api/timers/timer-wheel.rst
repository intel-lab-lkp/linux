.. SPDX-License-Identifier: GPL-2.0

===================================================================
Timer wheel and timer list timers - Implementation Details
===================================================================

The timer wheel is the infrastructure to handle all timer list
timers. Originally it was a cascading wheel and was reworked as a
non-cascading timer wheel back in 2016 with commit 500462a9de65 ("timers:
Switch to a non-cascading wheel").

Concept
=======

.. kernel-doc:: kernel/time/timer.c
   :doc: Concept of the timer wheel


Locking of timer bases
======================

.. kernel-doc:: kernel/time/timer.c
   :doc: Timer bases and hashed locking


NOHZ and timer bases
====================

.. kernel-doc:: kernel/time/timer.c
   :doc: NOHZ and timer bases


How to use timer list timers
============================

See also the users guide for how to use timer list timers. Details which are
important for the user are not listed here a second time to prevent duplicated
information (:doc:`timer-list-timers`).
