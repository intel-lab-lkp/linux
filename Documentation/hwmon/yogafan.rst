.. SPDX-License-Identifier: GPL-2.0-only

Kernel driver yogafan
=====================

Supported chips:

  * Lenovo Yoga, Legion, and IdeaPad Embedded Controllers
    Prefix: 'yogafan'
    Addresses: ACPI handle (see probe list in driver)

Author: Sergio Melas <sergiomelas@gmail.com>

Description
-----------

This driver provides fan speed monitoring for modern Lenovo laptops.
Most Lenovo laptops do not provide fan tachometer data through standard
ISA/LPC hardware monitoring chips. Instead, the data is stored in the
Embedded Controller (EC) and exposed via ACPI.

The driver implements a **Rate-Limited Lag (RLLag)** filter to handle
the low-resolution and jittery sampling found in Lenovo EC firmware.

Filter Details:
---------------

The RLLag filter is a discrete-time first-order lag model that ensures:
  - **Smoothing:** Jittery 1000-RPM step increments are smoothed into 1-RPM increments.
  - **Slew-Rate Limiting:** Prevents "teleporting" readings by capping the change
    to 1500 RPM/s, matching physical fan inertia.
  - **Polling Independence:** The filter math scales based on the time delta
    between userspace reads, ensuring the same physical curve regardless
    of whether you poll at 1Hz or 1000Hz.

Usage
-----

The driver exposes standard hwmon sysfs attributes:

================  =============================================================
Attribute         Description
================  =============================================================
fanX_input        Filtered fan speed in RPM.
================  =============================================================

Note: If the hardware reports 0 RPM, the filter is bypassed and 0 is reported
immediately to ensure the user knows the fan has stopped.
