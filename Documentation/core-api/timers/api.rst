.. SPDX-License-Identifier: GPL-2.0

==========
Timers API
==========

Working with jiffies
====================

.. kernel-doc:: include/linux/jiffies.h
   :identifiers: get_jiffies_64 jiffies_to_nsecs msecs_to_jiffies usecs_to_jiffies

.. kernel-doc:: kernel/time/time.c
   :no-identifiers: mktime64 set_normalized_timespec64 ns_to_timespec64 get_timespec64 put_timespec64 get_old_timespec32 put_old_timespec32 get_itimerspec64 put_itimerspec64 get_old_itimerspec32 put_old_itimerspec32

.. kernel-doc:: kernel/time/timer.c
   :identifiers: __round_jiffies __round_jiffies_relative round_jiffies round_jiffies_relative __round_jiffies_up __round_jiffies_up_relative round_jiffies_up round_jiffies_up_relative

Jiffie based time comparison helpers
====================================

.. kernel-doc:: include/linux/jiffies.h
   :doc: General information about time_* inlines

.. kernel-doc:: include/linux/jiffies.h
   :no-identifiers: time_comparision_disclaimer get_jiffies_64 jiffies_to_nsecs msecs_to_jiffies usecs_to_jiffies


Timespec related functions
==========================

.. kernel-doc:: kernel/time/time.c
   :identifiers: mktime64 set_normalized_timespec64 ns_to_timespec64 get_timespec64 put_timespec64 get_old_timespec32 put_old_timespec32 get_itimerspec64 put_itimerspec64 get_old_itimerspec32 put_old_itimerspec32


Handle timer list timers
========================

.. kernel-doc:: kernel/time/timer.c
   :identifiers: init_timer_key mod_timer_pending mod_timer timer_reduce add_timer add_timer_local add_timer_global add_timer_on timer_delete timer_shutdown try_to_del_timer_sync


Timeout and sleeping
====================

.. kernel-doc:: kernel/time/timer.c
   :identifiers: schedule_timeout msleep msleep_interruptible usleep_range_state

ktime_t operations
==================

.. kernel-doc:: include/linux/ktime.h
   :internal:


High-Resolution Timers
======================

.. kernel-doc:: include/linux/hrtimer.h
   :internal:

.. kernel-doc:: kernel/time/hrtimer.c
   :export:
