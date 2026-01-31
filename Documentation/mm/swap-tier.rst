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
-------

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
swap device is activated within a tier range, a reference is held from the
start of the tier to the priority of that swap device. This ensures that the
tier of region containing the active swap device does not disappear.

If a request to add a new tier with a priority higher than the current swap
device is received, the existing tier can be split.

However, specifying a tier in a cgroup does not hold a reference to the tier.
Consequently, the corresponding tier can disappear at any time.

Configuration Interface
-----------------------

The swap tiers can be configured via the following interface:

/sys/kernel/mm/swap/tiers

Operations can be performed using the following syntax:

* Add:    ``+"<tiername>":"<start_priority>"``
* Remove: ``-"<tiername>"``
* Modify: ``"<tiername>":"<start_priority>"``

Multiple operations can be provided in a single write, separated by spaces (" ")
or commas (",").

When configuring tiers, the specified value represents the **start priority**
of that tier. The end priority is automatically determined by the start
priority of the next higher tier. Consequently, adding or modifying a tier
automatically adjusts (splits or merges) the ranges of adjacent tiers to
ensure continuity.

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

**2. Modification and Splitting**

Here, 'HDD' is moved to start at 80, and a new tier 'SSD' is added at 100.
Notice how the ranges are automatically recalculated:
* 'SSD' takes the top range. Split HDD Tier's range. (100 to SHRT_MAX).
* 'HDD' is adjusted to the range between 'NET' and 'SSD' (80 to 99).
* 'NET' automatically extends to fill the gap below 'HDD' (-1 to 79).

::

    # echo "HDD:80, +SSD:100" > /sys/kernel/mm/swap/tiers
    # cat /sys/kernel/mm/swap/tiers
    Name             Idx   PrioStart   PrioEnd
    SSD              2     100         32767
    HDD              0     80          99
    NET              1     -1          79

**3. Removal**

Tiers can be removed using the '-' prefix.

::

    # echo "-SSD,-HDD,-NET" > /sys/kernel/mm/swap/tiers
