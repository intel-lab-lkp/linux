.. SPDX-License-Identifier: GPL-2.0

============================
Devlink Shared Instances
============================

Overview
========

Shared devlink instances allow multiple physical functions (PFs) on the same
chip to share an additional devlink instance for chip-wide operations. This
should be implemented within individual drivers alongside the individual PF
devlink instances, not replacing them.

The shared devlink instance should be backed by a faux device and should
provide a common interface for operations that affect the entire chip
rather than individual PFs.

Implementation
==============

Architecture
------------

The implementation should use:

* **Faux device**: Virtual device backing the shared devlink instance
* **Chip identification**: PFs are grouped by chip using a driver-specific identifier
* **Shared instance management**: Global list of shared instances with reference counting

Initialization Flow
-------------------

1. **PF calls shared devlink init** during driver probe
2. **Chip identification** using driver-specific method to determine device identity
3. **Lookup existing shared instance** for this chip identifier
4. **Create new shared instance** if none exists:

   * Create faux device with chip identifier as name
   * Allocate and register devlink instance
   * Add to global shared instances list

5. **Add PF to shared instance** PF list
6. **Set nested devlink instance** for the PF devlink instance

Cleanup Flow
------------

1. **Cleanup** when PF is removed; destroy shared instance when last PF is removed

Chip Identification
-------------------

PFs belonging to the same chip are identified using a driver-specific method.
The driver is free to choose any identifier that is suitable for determining
whether two PFs are part of the same device. Examples include VPD serial numbers,
device tree properties, or other hardware-specific identifiers.

Locking
-------

A global per-driver mutex protects the shared instances list and individual shared
instance PF lists during registration/deregistration.

Similarly to other nested devlink instance relationships, devlink lock of
the shared instance should be always taken after the devlink lock of PF.
