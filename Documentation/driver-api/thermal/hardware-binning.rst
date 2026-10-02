.. SPDX-License-Identifier: GPL-2.0

=====================
Thermal hardware bins
=====================

:Author: Amit Kucheria

Overview
========

Differences in SoC packaging let a vendor ship several SKUs built from the
same silicon but with different thermal characteristics.  Better packaging
dissipates more heat, so the temperature at which a SKU must be throttled (or
shut down) differs between SKUs.  This SKU-specific information is typically
burned into eFUSEs during manufacturing and can be read back at runtime.

Thermal "hardware binning" lets a single device tree describe the trip points
for every SKU of an SoC and have the kernel pick the values that match the
silicon it is actually running on.  The mechanism is modelled on the
``opp-supported-hw`` property used by the OPP framework: the platform supplies
one or more 32-bit hardware-version words decoded from fuses, and each trip
point declares the hardware bitmasks for which it is valid.

The feature is built when ``CONFIG_THERMAL_BIN`` is selected.  It depends on
``CONFIG_THERMAL_OF`` and is normally selected by the platform sensor driver
(for example the Qualcomm tsens driver) rather than chosen directly.

Device tree properties
=======================

Two properties are added to a trip-point node (see
``Documentation/devicetree/bindings/thermal/thermal-zones.yaml`` for the
authoritative schema):

``temperature-bin``
  An array of trip temperatures in millicelsius, one entry per
  ``thermal-hw-bin`` sub-group.  It is used *instead of* the plain
  ``temperature`` property when the trip temperature depends on the hardware
  bin.  Exactly one of ``temperature`` or ``temperature-bin`` must be present
  in a trip node.

``thermal-hw-bin``
  One or more groups of N 32-bit masks describing the hardware revisions for
  which the trip point is valid, where N is the number of hardware-version
  words the platform registered (``supported_hw_bin_count``, see below).  The
  format and semantics match ``opp-supported-hw``: a sub-group matches when,
  for every word ``i`` in ``[0 .. N-1]``, the bitwise AND of the ``i``-th mask
  and the ``i``-th platform hardware value is non-zero.  The trip is enabled if
  any sub-group matches.  When paired with ``temperature-bin`` the number of
  sub-groups must equal the number of ``temperature-bin`` entries, and the
  matching sub-group selects which ``temperature-bin`` entry is used.  When
  ``temperature-bin`` is present, ``thermal-hw-bin`` is required.

Example::

    trips {
        cpu-critical {
            /* bin 0 throttles at 95C, bin 1 at 105C */
            temperature-bin = <95000 105000>;
            thermal-hw-bin = <0x1>, <0x2>;
            hysteresis = <1000>;
            type = "critical";
        };
    };

Here the platform registers a single hardware-version word (N = 1).  If that
word ANDed with ``0x1`` is non-zero, the first sub-group matches and the trip
uses 95000 mC; if it ANDed with ``0x2`` is non-zero, the second sub-group
matches and the trip uses 105000 mC.  If neither matches the trip is skipped.

Platform driver API
===================

A platform driver decodes the SoC fuses into hardware-version words and hands
them to the thermal core, which makes them available to the OF trip parser.
The words are carried in ``struct thermal_hw_bin_info``::

    struct thermal_hw_bin_info {
            const u32       *supported_hw_bin;
            unsigned int     supported_hw_bin_count;
    };

``supported_hw_bin``
  Array of N bitfields identifying the running hardware version.

``supported_hw_bin_count``
  Number of valid entries in ``supported_hw_bin`` (the N above).  Must be in
  the range ``1 .. THERMAL_HW_BIN_MAX_LEVELS``.

There are two ways to supply this information:

At registration time
  Pass a filled ``struct thermal_hw_bin_info`` to
  ``devm_thermal_of_zone_register_with_bin()``.  This is the preferred path
  when the platform driver registers its own zones and already knows the
  hardware version, because the bin info is in place before the trips are
  parsed::

      struct thermal_zone_device *
      devm_thermal_of_zone_register_with_bin(struct device *dev, int id,
                                             void *data,
                                             const struct thermal_zone_device_ops *ops,
                                             const struct thermal_hw_bin_info *hw_bin_info);

After registration
  Call ``thermal_zone_set_supported_hw_bin()`` (or the devm-managed
  ``devm_thermal_zone_set_supported_hw_bin()``) on an already-registered
  zone::

      int thermal_zone_set_supported_hw_bin(struct thermal_zone_device *tz,
                                            const u32 *vers, unsigned int count);
      int devm_thermal_zone_set_supported_hw_bin(struct device *dev,
                                                 struct thermal_zone_device *tz,
                                                 const u32 *vers, unsigned int count);

  Both copy ``vers`` into storage owned by the zone's parent device (the copy
  is freed via devm when that device is unbound), so the caller's array need
  not outlive the call.  ``count`` must be ``1 .. THERMAL_HW_BIN_MAX_LEVELS``
  and the zone must have a parent device.

  ``thermal_zone_get_hw_bin_info()`` returns the bin info previously set on a
  zone, or NULL if none has been set.

Concurrency
===========

Bin info is published once, at probe, before the zone's trips are parsed, and
cleared once at unbind.  The ``supported_hw_bin`` pointer is the publish gate:
``thermal_zone_set_supported_hw_bin()`` stores it with ``smp_store_release()``
after filling the array and count, and ``thermal_zone_get_hw_bin_info()`` reads
it with ``smp_load_acquire()``.  A reader that observes a non-NULL pointer is
therefore guaranteed to see a consistent ``supported_hw_bin_count`` and array
and may dereference them without further barriers.  There is no lock guarding
concurrent writers: drivers must not race two set/clear operations against each
other.

When a build does not have ``CONFIG_THERMAL_BIN`` enabled, the
``thermal_zone_set_supported_hw_bin()`` family returns ``-EOPNOTSUPP`` and
``thermal_zone_get_hw_bin_info()`` returns NULL via inline stubs, so callers
compile unconditionally.

If a trip uses ``temperature-bin`` but no bin info has been registered for the
zone, the core cannot select an entry; it warns once and falls back to the
first ``temperature-bin`` value.  Platform drivers should therefore register
the bin info before the zone's trips are parsed.

The Qualcomm tsens driver is the reference user: it reads the named nvmem
cells described by a per-SoC ``struct tsens_hw_bin_desc``, converts the raw
fuse values into hardware-version words, and passes the result to
``devm_thermal_of_zone_register_with_bin()``.
