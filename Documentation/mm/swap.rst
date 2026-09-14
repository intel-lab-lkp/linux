.. SPDX-License-Identifier: GPL-2.0

====
Swap
====

Offload-only swap areas
-----------------------

``SWAP_FLAG_OFFLOAD_ONLY`` marks a swap area as a destination for explicit
userspace proactive reclaim.  The stable qualifying interfaces are cgroup v2
``memory.reclaim`` and ``/sys/devices/system/node/nodeX/reclaim``.  The swap
allocator excludes such an area from kswapd, direct reclaim, and other
pressure-driven swap allocation.  Normal swap priority ordering still applies
among the areas eligible for the current reclaim context.

The MGLRU debugfs eviction interface currently establishes the same internal
proactive-reclaim provenance and can therefore use an offload-only area.
Debugfs is not a stable userspace ABI, however, so that behaviour is not part
of this interface's permanent contract.

This permits a system to combine a small conventional swap area, which is
engineered for forward progress in emergency reclaim, with a larger or more
complex area used for ordinary cold-page offload.  For example, the latter may
be RAM-compressed or may use a filesystem with compression, checksums, or
redundancy.  Making every such write path safe in direct reclaim can require
backend-specific reserves, preallocation, non-blocking allocation, and
recursion rules.  Excluding new pressure-reclaim writes can reduce that
requirement and its complexity.

For a RAM-compressed area such as zram, unused logical slots also do not imply
that enough physical memory remains to store their future contents.  Static
swap priority cannot express that distinction or provide late fallback after a
selected area's write fails.

The policy is attached to an activated swap area, not to its underlying
physical storage.  A raw swap partition and a filesystem swapfile on the same
device are separate areas and may use different policies.  The kernel does not
infer this policy from the block driver, filesystem, or swap priority.

This is a reclaim-provenance policy, not a measurement of current memory
headroom.  Userspace should only request proactive offload while its own
watermark or PSI policy considers memory pressure low.

DAMON reclaim and ``MADV_PAGEOUT`` do not currently establish the proactive
reclaim context, so they cannot allocate slots from an offload-only area.
Offload-only areas are also ineligible for hibernation image allocation.

Offload-only areas bypass zswap stores.  Zswap writeback may run after the
proactive context which selected the slot has ended, so admitting the folio to
zswap would otherwise defer the backend write beyond that context.  The
hierarchical cgroup v2 ``memory.zswap.writeback=0`` policy remains
authoritative: when zswap is enabled, it also refuses direct proactive writes
to an offload-only area.  Marking an area offload-only does not override a
cgroup policy which disables all swapping attempts to devices.

The flag controls both allocation of new swap slots and newly initiated
non-zero backend writes.  A folio can retain its swap entry after swapin.  If
ordinary reclaim later tries to rewrite such an offload-only entry, the VM
redirties and activates the folio instead; proactive reclaim may retry the
write.  Zero-filled folios may still update the in-memory swap zeromap without
backend I/O.

The flag does not prevent reads, swapoff, or writes which are already queued or
in flight.  It therefore does not by itself provide a forward progress
guarantee for an I/O path which allocates memory: earlier writes must still be
able to complete, and reads must remain reclaim-safe.  Repeatedly refusing
retained-entry writes can also reduce reclaim efficiency and lead to OOM while
the dirty folios remain resident.

Page-cluster discard is incompatible with an offload-only area because its
work item can run after the context which freed the entries has ended.  Swapon
therefore rejects a resolved page-cluster discard policy combined with
``SWAP_FLAG_OFFLOAD_ONLY``.  Swapon-time discard is permitted because it
completes synchronously during activation.  The existing discard precedence
still applies: requesting both discard-once and discard-pages selects
discard-once.  Discard requests which the swap area does not support remain
ignored.

Architecture-specific swap metadata preparation still runs before a retained
write is refused, so that metadata remains coherent with the dirty resident
folio.  This policy controls swap-backend I/O; it does not promise that core VM
or architecture preparation performs no allocation.

``/proc/vmstat`` reports ``swpout_offload_refused`` in base pages.  The counter
advances when ordinary reclaim refuses a newly initiated write through a
retained offload-only entry.  It can reveal repeated activation of such folios,
but it is not a count of skipped areas during new-slot allocation.

An offload-only area should therefore be configured with a reclaim-safe swap
area as fallback.  If no eligible swap space remains, swap allocation fails
and the existing reclaim and OOM policy applies.
