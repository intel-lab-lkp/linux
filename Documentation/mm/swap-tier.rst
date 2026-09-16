.. SPDX-License-Identifier: GPL-2.0

:Author: Chris Li <chrisl@kernel.org>,
         Youngjun Park <youngjun.park@lge.com>

==========
Swap Tier
==========

Swap tier is a group of swap devices that share a priority. It acts as a
facilitation layer, allowing users to manage swap devices based on their
speeds.

Users are encouraged to assign swap device priorities according to device
speed to fully utilize this feature.

Tier Index
----------

A tier is created when the first swap device with its priority is swapped on,
and removed when the last one is swapped off. Each tier is given an index when
it is created and keeps it until it is removed, so a tier's index does not
change when another priority is swapped on or off.

Per-cgroup Tier Selection
-------------------------

A memory cgroup can be limited to some tiers through debugfs. This is for
evaluation, not a stable ABI.

``/sys/kernel/debug/swap/tiers`` lists the index and priority of each tier.
``/sys/kernel/debug/swap/memcg_tiers`` takes a cgroup path and a mask in hex,
where bit ``i`` allows the tier at index ``i``::

    # cat /sys/kernel/debug/swap/tiers
    Idx   Prio
    0     100
    1     50
    # echo "/batch 0x2" > /sys/kernel/debug/swap/memcg_tiers

There is no separate delete operation. Writing a mask that allows every tier
clears the restriction, so the cgroup drops out of the file::

    # echo "/batch 0xffffffff" > /sys/kernel/debug/swap/memcg_tiers

A cgroup's mask is also dropped when the cgroup is removed.

A mask applies to the memory charged to its own cgroup and is not inherited by
child cgroups. A tier keeps its index for its lifetime, so the same mask keeps
selecting the same tier across a swapon or swapoff.

When a tier's last device is swapped off, its index is freed and can be reused
by a later tier. The freed index is re-allowed in every cgroup mask, so a
cgroup that had disabled it must disable it again once a new tier reuses the
index.
