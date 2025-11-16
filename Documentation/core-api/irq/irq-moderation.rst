.. SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

===========================================
Platform wide software interrupt moderation
===========================================

:Author: Luigi Rizzo <lrizzo@google.com>

.. contents:: :depth: 2

Introduction
------------

Platform Wide software interrupt moderation is a variant of moderation
that adjusts the delay based on platform-wide metrics, instead of
considering each source separately.  It then uses hrtimers to implement
adaptive, per-CPU moderation in software, without requiring any specific
hardware support other than Pending Bit Array, a standard feature
of MSI-X.

The most common and robust implementation of moderation enforces
some minimum **delay** between subsequent interrupts, using a timer
in the device or in software. Most NICs support programmable hardware
moderation, with timer granularity down to 1us or so.  NVME also
specifies hardware moderation timers, with 100us granularity.

One downside of moderation, especially with **fixed** delay, is that
even with moderate load, the notification latency can increase by as
much as the moderation delay. This is undesirable for transactional
workloads. At high load the extra delay is less problematic, because
the queueing delays that occur can be one or more orders of magnitude
bigger.

To address this problem, software can dynamically adjust the delay, making
it proportional to the I/O rate. This is called **adaptive** moderation,
and it is commonly implemented in network device drivers.

There is one aspect that per-source moderation does not address.

Several Systems-on-Chip (SoC) from all vendors (Intel, AMD, ARM), show
huge reduction in I/O throughput (up to 3-4x times slower for high speed
NICs or SSDs) in presence of high MSI-X interrupt rates across the entire
platform (1-3M intr/s total, depending on the SoC). Note that unaffected
SoCs can sustain 20-30M intr/s from MSI-X without performance degradation.

The above performance degradation is not caused by overload of individual
CPUs. What matters is the total MSI-X interrupt rate, across either
individual PCIe root port, or the entire SoC. The specific root cause
depends on the SoC, but generally some internal block (in the PCIe root
port, or in the IOMMU block) applies excessive serialization around
MSI-X writes. This in turn causes huge delays in other PCIe transactions,
leading to the observed performance drop.

PLATFORM WIDE ADAPTIVE MODERATION
---------------------------------

Platform-Wide adaptive interrupt moderation addresses the problem
operateing as follows (all parameters are configurable via module parameters
irq_moderation.${name}=${value} or /proc/irq/soft_moderation):

* On each interrupt, increments a per-CPU interrupt counter.

* Opportunistically, every ``update_msi`` or so, each CPU scalably
  accumulates the values across the entire system, computes the global
  and per-CPU interrupt rate, and the number of CPUs actively processing
  interrupts, ``active_cpus``.

* Based on a configurable ``target_irq_rate``, each CPU the per-CPU
  fair share (``target_irq_rate / active_cpus``) and whether the global
  and total rate are abovethe targets. A simple control loop then adjusts
  up/down its per-CPU moderation delay, ``mod_ns``, between 0 (disabled)
  and a configurable maximum ``delay_us``.

* When ``mod_ns`` is above a threshold (e.g. 10us), the first interrupt
  served by that CPU starts an hrtimer to fire ``mod_ns`` nanoseconds.
  All interrupts sources served by that CPU will be disabled as they come.

* When the timer fires, all disabled sources are re-enabled, allowing pending
  interrupts to be processed again.

This scheme is effective in keeping the total interrupt rate under
control as long as the configuration parameters are sensible
(``delay_us < #CPUs / target_irq_rate``).

It also lends itself to some extensions, specifically:

* **protect against hardirq overload**. It is possible for one CPU
  handling interrupts to be overwhelmed by hardirq processing. The
  control loop can be extended to declare an overload situation when the
  percentage of time spent in hardirq is above a configurable threshold
  ``hardirq_percent``. Moderation can thus kick in to keep the load within bounds.

* **reduce latency using timer-based polling**. Similar to ``napi_defer_hard_irq``
  described earlier, once interrupts are disabled and we have an hrtimer active,
  we keep the timer active for a few rounds and run the handler from a timer callback
  instead of waiting for an interrupt. The ``timer_rounds`` parameter controls this behavior,

  Say the control loop settles on 120us delay to stay within the global MSI-X rate limit.
  By setting ``timer_rounds=2``, each time we have a hardware interrupt, the handler
  will be called two more times by the timer. As a consequence, in the same conditions,
  the same global MSI-X rate will be reached with just 120/3 = 40us delay, thus improving
  latency significantly (note that those handlers call do cause extra CPU work, so we
  may lose some of the efficiency gains coming from large delays).

CONFIGURATION
-------------

Configuration of this system is done via module parameters
``irq_moderation.${name}=${value}`` (for boot-time defaults)
or writing ``echo "${name}=${value}" > /proc/irq/soft_moderation``
for run-time configuration.

Here are the existing module parameters

* ``delay_us`` (0: off, range 0-500)

   The maximum moderation delay. 0 means moderation is globally disabled.

* ``target_irq_rate`` (0 off, range 0-50000000)

  The maximum irq rate across the entire platform. The adaptive algorithm will adjust
  delays to stay within the target. Use 0 to disable this control.

* ``hardirq_percent`` (0 off, range 0-100)

  The maximum percentage of CPU time spent in hardirq. The adaptive algorithm will adjust
  delays to stay within the target. Use 0 to disable this control.

* ``timer_rounds`` (0 0ff, range 0-20)

  Once the moderation timer is activated, how many extra timer rounds to do before
  re-enabling interrupts.

* ``update_ms`` (default 1, range 1-100)

  How often the adaptive control should adjust delays. The default value (1ms) should be good
  in most circumstances.

Interrupt moderation can be enabled/disabled on individual IRQs as follows:

* module parameter ``${driver}.soft_moderation=1`` (default 0) selects
  whether to use moderation at device probe time.

* ``echo 1 > /proc/irq/*/${irq_name}/../soft_moderation`` (default 0, disabled) toggles
  moderation on/off for specific IRQs once they are attached.

**INTEL SPECIFIC**

Recent intel CPUs support a kernel feature, enabled via boot parameter ``intremap=posted_msi``,
that routes all interrupts targeting one CPU via a special interrupt, called **posted_msi**,
whose handler in turn calls the individual interrupt handlers.

The ``posted_msi`` kernel feature always uses moderation if enabled (``delay_us > 0``) and
individual IRQs do not need to be enabled individually.
