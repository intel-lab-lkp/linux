.. SPDX-License-Identifier: GPL-2.0

====================================
Arm CPU prefetch modulation controls
====================================

``ARM64_CPUMOD`` exposes selected CPU implementation control register fields
through per-CPU sysfs attributes under::

  /sys/devices/system/cpu/cpuN/cpumod/

The ``cpumod`` directory is created only when the CPU MIDR matches a recognized
Grace or Vera profile. CPUs with unsupported MIDRs are skipped without blocking
module load or CPU hotplug and do not have a ``cpumod`` directory.

The interface is intended for controlled performance characterization and
evaluation. It is not intended as a general production tuning ABI, and the
configuration should remain disabled by default on production systems.

Configuration and placement
===========================

The controls are built when ``CONFIG_ARM64_CPUMOD`` is enabled. The code lives
under ``arch/arm64/kernel/`` because the exposed state is CPU implementation
control state accessed by the arm64 kernel, similar in placement to other
architecture CPU-facing helpers.

The ABI remains separate from the existing arm64 ``cpu*/regs`` sysfs files:
``cpumod`` exposes a small set of named, range-checked control fields rather
than a general raw register dump.

CPU hotplug
===========

A ``cpumod`` directory is created only for online CPUs with a supported
profile. The directory is removed when a CPU goes offline and recreated when
the CPU comes back online, again only when its MIDR matches a supported
profile. Reads and writes require the target CPU to be online.

Firmware requirement
====================

The controls require firmware to permit EL1 reads and writes to the relevant
CPU implementation control registers. On systems where firmware traps or
blocks those accesses, the interface cannot be used.

Current ABI
===========

Common attributes:

``affected_cpus``
  Read-only decimal CPU identifier for the sysfs instance.

``pf_dis``
  Hardware prefetch disable control. Valid values are ``0`` and ``1``.

``pf_mode``
  Hardware prefetch aggressiveness mode. Valid values are ``0`` through ``9``.
  Values ``10`` through ``15`` are reserved and rejected.

Grace-only attributes:

``cbusy_filter_threshold``
  Valid values are ``0`` through ``3``.

``cbusy_filter_window``
  Valid values are ``0`` through ``3``.

``cmc_min_ways``
  Valid values are ``0`` through ``7``.

Vera-only attributes:

``l2spr_cmc_max_ways``
  Valid values are ``0`` through ``7``.

Invalid writes
==============

Writes outside the documented range fail with ``-EINVAL`` before the cached
sysfs state or remote CPU register state is updated.

Open register inventory
=======================

The first RFC intentionally exposes only the currently validated subset. A
complete documented-field inventory, including L2CDP, should be reviewed before
adding further ABI nodes.
