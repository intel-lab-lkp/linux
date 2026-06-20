.. SPDX-License-Identifier: GPL-2.0

:Author: Chris Li <chrisl@kernel.org> Youngjun Park <youngjun.park@lge.com>

==========
Swap Tier
==========

Swap tier is a collection of user-named groups classified by priority ranges.
It acts as a facilitation layer, allowing users to manage swap devices based
on their speeds.

Users are encouraged to assign swap device priorities according to device
speed to fully utilize this feature. While the current implementation is
integrated with cgroups, the concept is designed to be extensible for other
subsystems in the future.

Use case
---------

Users can perform selective swapping by choosing a swap tier assigned according
to speed within a cgroup.

For more information on cgroup v2, please refer to
``Documentation/admin-guide/cgroup-v2.rst``.

Priority Range
--------------

The specified tiers must cover the entire priority range from -1
(DEF_SWAP_PRIO) to SHRT_MAX.

Consistency
-----------

Tier consistency is guaranteed with a focus on maximizing flexibility. When a
swap device is activated within a tier range, the tier covering that device's
priority is guaranteed not to disappear or change while the device remains
active. Adding a new tier may split the range of an existing tier, but the
active device's tier assignment remains unchanged.

However, specifying a tier in a cgroup does not guarantee the tier's existence.
Consequently, the corresponding tier can disappear at any time.

Configuration Interface
-----------------------

The swap tiers can be configured via the following interface:

/sys/kernel/mm/swap/tiers

Operations can be performed using the following syntax:

* Add:    ``+"<tiername>":"<start_priority>"``
* Remove: ``-"<tiername>"``

Tier names must consist of alphanumeric characters and underscores. Multiple
operations can be provided in a single write, separated by commas (",") or
whitespace (spaces, tabs, newlines).

When configuring tiers, the specified value represents the **start priority**
of that tier. The end priority is automatically determined by the start
priority of the next higher tier. Consequently, adding a tier
automatically adjusts the ranges of adjacent tiers to ensure continuity.

Examples
--------

**1. Initialization**

A tier starting at -1 is mandatory to cover the entire priority range up to
SHRT_MAX. In this example, 'HDD' starts at 50, and 'NET' covers the remaining
lower range starting from -1.

::

    # echo "+HDD:50, +NET:-1" > /sys/kernel/mm/swap/tiers
    # cat /sys/kernel/mm/swap/tiers
    Name             Idx   PrioStart   PrioEnd
    HDD              0     50          32767
    NET              1     -1          49

**2. Adding a New Tier (split)**

A new tier 'SSD' is added at priority 100, splitting the existing 'HDD' tier.
The ranges are automatically recalculated:

* 'SSD' takes the top range (100 to SHRT_MAX).
* 'HDD' is adjusted to the range between 'NET' and 'SSD' (50 to 99).
* 'NET' remains unchanged (-1 to 49).

::

    # echo "+SSD:100" > /sys/kernel/mm/swap/tiers
    # cat /sys/kernel/mm/swap/tiers
    Name             Idx   PrioStart   PrioEnd
    SSD              2     100         32767
    HDD              0     50          99
    NET              1     -1          49

**3. Removal (merge)**

Tiers can be removed using the '-' prefix.
::

    # echo "-SSD" > /sys/kernel/mm/swap/tiers

When a tier is removed, its priority range is merged into the adjacent
tier. The merge direction is always upward (the tier below expands),
except when the lowest tier is removed — in that case the tier above
shifts its starting priority down to -1 to maintain full range coverage.

::

    Initial state:
    Name             Idx   PrioStart   PrioEnd
    SSD              2     100         32767
    HDD              1     50          99
    NET              0     -1          49

    # echo "-SSD" > /sys/kernel/mm/swap/tiers

    Name             Idx   PrioStart   PrioEnd
    HDD              1     50          32767       <- merged with SSD's range
    NET              0     -1          49

    # echo "-NET" > /sys/kernel/mm/swap/tiers

    Name             Idx   PrioStart   PrioEnd
    HDD              1     -1          32767       <- shifted down to -1

**4. Interaction with Active Swap Devices**

If a swap device is active (swapon), the tier covering that device's
priority cannot be removed. Splitting the active tier's range is only
allowed above the device's priority.

Assume a swap device is active at priority 60 (inside 'HDD' tier).

::

    # swapon -p 60 /dev/zram0

    Name             Idx   PrioStart   PrioEnd
    HDD              0     50          32767
    NET              1     -1          49

    # echo "-HDD" > /sys/kernel/mm/swap/tiers
    -bash: echo: write error: Device or resource busy

    # echo "+SSD:60" > /sys/kernel/mm/swap/tiers
    -bash: echo: write error: Device or resource busy

    # echo "+SSD:100" > /sys/kernel/mm/swap/tiers

    Name             Idx   PrioStart   PrioEnd
    SSD              2     100         32767
    HDD              0     50          99          <- device (prio 60) stays here
    NET              1     -1          49
