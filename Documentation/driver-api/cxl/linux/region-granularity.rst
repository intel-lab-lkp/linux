.. SPDX-License-Identifier: GPL-2.0

==============================================
Region Granularity and Multi-Level Interleave
==============================================

CXL memory regions stripe accesses across one or more endpoints by
routing transactions through a tree of decoders: a root decoder, zero
or more switch decoders, and an endpoint decoder at each leaf. Each
decoder in that path uses a contiguous range of HPA address bits to
pick its target. This document describes how those bit ranges combine
across levels, the constraints CXL Spec 4.0 places on legal
combinations, and how the Linux CXL driver validates and programs
them.

The relevant spec material is CXL Spec 4.0 Section 9.13.1, especially
Section 9.13.1.1 ("Legal Interleaving Configurations") and CXL Spec
4.0 Tables 9-6, 9-7, and 9-8. CXL Spec 4.0 Figure 9-17 (two-level)
and Figure 9-18 (three-level) illustrate the bit-routing intuition
this document builds on.


HPA Selector Bits
=================

For a single interleave level with ``W`` ways and granularity ``G``
bytes, the **selector bits** are the ``log2(W)`` HPA bits the decoder
inspects to choose among its ``W`` targets. The lowest of those bits
sits at position ``log2(G)``, so the selector spans
``[log2(G) + log2(W) - 1 : log2(G)]``. Equivalently, the bitmask is
``(W - 1) * G``.

A few examples make the geometry concrete::

   2-way at  256B  ->  selector bit 8         (one bit)
   4-way at  1KB   ->  selector bits[11:10]   (two bits)
   2-way at  4KB   ->  selector bit 12        (one bit)
   8-way at  1KB   ->  selector bits[12:10]   (three bits)

Throughout this document "inner bit" means a lower-numbered HPA bit
(smaller stride, faster cycling among targets) and "outer bit" means
a higher-numbered HPA bit (larger stride, slower cycling). HPA[8] is
the innermost legal CXL selector bit; HPA[14] is the outermost.


The Multi-Level Rule
====================

When a region's interleave is split across multiple decoder levels,
CXL Spec 4.0 Section 9.13.1 requires that:

   "all the levels use different, but consecutive, HPA bits to
   select the target and no Interleave Set has more than eight
   devices."

Two requirements live in that sentence:

* **Disjoint** - each level's selector bits must not overlap any
  other level's bits.
* **Consecutive** - the union of all level selectors must be a single
  contiguous run of HPA bits with no gap.

The union of level selectors is the **region selector**, and it must
equal ``(region_ways - 1) * region_gran``. Because that mask is itself
a contiguous run by construction, the levels just need to partition
it cleanly.

The seven legal interleave granularity values split into two groups
(see CXL Spec 4.0 Section 9.13.1 and Table 8-116):

* **Group 1** - 256B, 512B, 1024B, 2048B (interleaving on HPA[8..11])
* **Group 2** - 4096B, 8192B, 16384B (interleaving on HPA[12..14])

CXL host bridges and switches must support every value in both
groups. CXL memory devices must support at least one group, advertised
through their HDM Decoder Capability register.


Same-Granularity Interleave
===========================

The simplest case is ``region_gran == root_gran``. Each level's
granularity equals the next inner level's IG times that level's
ways. The region selector bits stack inside-out from the root.

Example: 4-way region on a 2-way root, 256B everywhere::

   Level    Ways   Granularity   Selector bits
   ------   ----   -----------   -------------
   Root     2      256B          bit 8
   Switch   2      512B          bit 9
   Region   4      256B          bits[9:8]

Each level's IG sits one bit position above the next inner level's
selector. The region's effective selector spans ``[9:8]``, two bits,
matching ``(4 - 1) * 256``.


Mixed-Granularity Interleave (Power-of-2)
=========================================

A region's granularity may be **less than** the root decoder's
granularity. In that case the root claims an outer selector bit and
the switches beneath it claim inner bits. The arrangement is legal
whenever both granularities are powers of 2 and the resulting bits
remain disjoint and consecutive.

Two-Level Example (CXL Spec 4.0 Figure 9-17)
--------------------------------------------

8-way region, 2-way root at 4KB, 4-way switch at 1KB::

   Level    Ways   Granularity   Selector bits
   ------   ----   -----------   -------------
   Root     2      4KB           bit 12
   Switch   4      1KB           bits[11:10]
   Region   8      1KB           bits[12:10]

The host bridge bisects the address range at 4KB granularity, sending
one half to each root port. Beneath each root port a 4-way switch
splits its half into four 1KB chunks. To each endpoint the region
appears as an 8-way interleave at 1KB based on bits[12:10]; the
leftmost endpoint receives 1KB chunks at HPA bases 0, 8KB, 16KB, ...

Three-Level Example (CXL Spec 4.0 Figure 9-18)
----------------------------------------------

8-way region across cross-HB / HB / switch, all 2-way::

   Level         Ways   Granularity   Selector bits
   -----------   ----   -----------   -------------
   Cross-HB      2      4KB           bit 12
   Host bridge   2      2KB           bit 11
   Switch        2      1KB           bit 10
   Region        8      1KB           bits[12:10]

Three levels of 2-way interleave each contribute one bit. Total
endpoints: ``2 * 2 * 2 = 8``. Selector union:
``bit 12 | bit 11 | bit 10`` = ``bits[12:10]``. Disjoint and
consecutive.

General Power-of-2 Rules
------------------------

For power-of-2 ways at every level:

* ``region_gran <= root_gran`` is allowed; ``region_gran > root_gran``
  is not (the root's selector would land inside one region stride).
* Both granularities must be powers of 2.
* The ratio ``root_gran / region_gran`` is the number of inner
  position slots packed beneath one root target. Endpoint position is
  partitioned by root target rather than interleaved across them.
* Mixed-granularity is always optional for power-of-2 topologies. A
  same-granularity arrangement always exists for the same set of
  endpoints. Firmware may prefer mixed-granularity to expose a finer
  switch-level interleave to the CPU.


3-Way Family Interleave
=======================

CXL Spec 4.0 Section 9.13.1.1 defines a non-power-of-2 family of
interleave widths: 3, 6, and 12. These are not pure bit selectors.
A 3-way root divides the address by 3 in value-space (modulo or XOR
arithmetic, see CXL Spec 4.0 Section 9.18) rather than picking a
target from a contiguous bit range. As a consequence:

* A strict 3-way root consumes **zero** HPA selector bits of its own.
* Only the power-of-2 component of the region width contributes
  selector bits. For a 6-way region, the bit-selector component is
  2-way; for 12-way, 4-way.
* Mixed-granularity is **required** for any 6-way or 12-way region on
  a 3-way root. There is no same-granularity alternative.

Legal 3-Way-Family Configurations
---------------------------------

CXL Spec 4.0 Tables 9-6, 9-7, and 9-8 enumerate the legal
configurations. IGB is the device-level interleave granularity.

Table 9-6: 12-way device-level interleave at IGB::

   Cross-host bridge   Root complex     CXL switch
   -----------------   --------------   --------------
   12-way at IGB       no interleave    no interleave
   6-way  at 2*IGB     2-way at IGB     no interleave
   6-way  at 2*IGB     no interleave    2-way at IGB
   3-way  at 4*IGB     4-way at IGB     no interleave
   3-way  at 4*IGB     no interleave    4-way at IGB
   3-way  at 4*IGB     2-way at IGB     2-way at 2*IGB
   3-way  at 4*IGB     2-way at 2*IGB   2-way at IGB

Table 9-7: 6-way device-level interleave at IGB::

   Cross-host bridge   CXL host bridge   CXL switch
   -----------------   ---------------   --------------
   6-way  at IGB       no interleave     no interleave
   3-way  at 2*IGB     2-way at IGB      no interleave
   3-way  at 2*IGB     no interleave     2-way at IGB

Table 9-8: 3-way device-level interleave at IGB::

   Cross-host bridge   CXL host bridge   CXL switch
   -----------------   ---------------   --------------
   3-way  at IGB       no interleave     no interleave

Span Identity for 3-Way-Family Roots
------------------------------------

For a 3-way (or 6-way, 12-way) root, the region must occupy exactly
one full cycle of the root's divide-by-3 pattern::

   region_ways * region_gran == root_ways * root_gran

Every legal row of CXL Spec 4.0 Tables 9-6, 9-7, and 9-8 satisfies
this identity.

Why power-of-2 roots do not need this check explicitly, and why
3-way roots do, comes down to how each cycles through its targets:

* A power-of-2 root selects targets purely by HPA bits. The bits it
  owns are part of the region selector mask
  ``(region_ways - 1) * region_gran``. The disjoint and containment
  checks on selector bits inside the region selector force
  ``root_ways * root_gran`` to be a power-of-2 divisor of
  ``region_ways * region_gran``, and the region size constraint
  forces it to be exactly one such divisor. Span equality is a
  by-product of bit accounting.

* A 3-way root divides the address by 3 in value-space. It cycles
  through three targets at every ``root_gran``-byte stride, but
  consumes no HPA selector bits at all. The selector mask returned
  by ``get_selector()`` for a strict 3-way is zero. Selector bit
  accounting cannot see the address space the root reserves at
  ``log2(root_gran)``, so it cannot rule out arrangements where
  ``region_ways * region_gran`` is some multiple of
  ``root_ways * root_gran`` other than one. (For example, a
  same-granularity 6-way region on a 3-way root would pass selector
  checks while failing to cover one root cycle.)

The span identity restores that constraint for non-power-of-2 roots.
The driver enforces it as a pre-flight check in
``cxl_region_attach()``, before the per-port selector walk runs.
A failed span check is rejected with the dmesg message
``region ways*gran (W*G) != root ways*gran (W*G)``.

Mixed-Granularity Rules for 3-Way Roots
---------------------------------------

The general power-of-2 mixed-gran rules apply to the 3-way family
with two adjustments:

* ``region_gran <= root_gran`` is allowed; ``region_gran > root_gran``
  is not. (Same as power-of-2.)
* Both granularities must be powers of 2. (Same as power-of-2; the
  3-way component is in the *ways*, not the granularity.)
* The ratio ``root_gran / region_gran`` is the number of inner
  position slots packed beneath one root target. (Same as
  power-of-2; the value-space root advances by ``root_gran``
  whether or not it consumes selector bits.)
* ``region_ways * region_gran == root_ways * root_gran`` must hold.
  (3-way only. Power-of-2 roots get this for free from selector
  containment.)
* Mixed-granularity is **required** for any 6-way or 12-way region
  on a 3-way root, because no same-granularity arrangement can hold
  the span identity. (3-way only; for power-of-2 mixed-gran is
  optional.)


Position Arithmetic
===================

Position is the index ``[0, region_ways)`` that orders an endpoint
within the region. The driver derives an endpoint's position by
walking the topology from endpoint up to root.

For same-granularity regions the standard recurrence applies at every
level::

   pos = pos * parent_ways + parent_pos

The root's targets cycle at the innermost selector bit, so the root
contributes the position MSBs naturally.

For mixed-granularity regions the root's targets cycle more slowly
than the switches' targets - by exactly ``stride = root_gran /
region_gran`` inner positions per root target. Endpoint positions
within one root target are packed consecutively, so the root's
contribution is rescaled at the root boundary::

   pos = pos + stride * parent_pos     (root iteration only)

For a 3-way root the same packing applies; ``stride`` is still
``root_gran / region_gran`` whenever the granularities differ.

Peer-Distance and the check_last_peer Walk
------------------------------------------

When two endpoints share a downstream port at some intermediate
level, ``check_last_peer()`` verifies that the peer at
``pos - peer_distance`` lives behind the same dport as the endpoint
being attached. ``peer_distance`` is the number of region positions
between two consecutive endpoints that share a dport at the port
under inspection.

The seed of ``peer_distance`` depends on the layout the position
recurrence builds:

* Same-granularity layout (``stride == 1``) is multiplicative at the
  root: ``pos = pos * root_ways + parent_pos``. Changing the root's
  ``parent_pos`` by 1 shifts each inner-position group by ``root_ways``
  region positions, so peer distance at any sub-root port carries a
  factor of ``root_ways``. Seed: ``root_ways``.

* Mixed-granularity layout (``stride > 1``) is additive at the root:
  ``pos = pos + stride * parent_pos``. Changing the root's
  ``parent_pos`` shifts every endpoint by the same constant ``stride``;
  peer distances among endpoints under one root target do not depend
  on ``root_ways``. Seed: ``1``.

Inner ports contribute their ``nr_targets`` multiplicatively as the
walk descends, and the current port's ``iw`` folds in at the end.

Example: 6-way region on a 3-way root (CXL Spec 4.0 Table 9-7)
--------------------------------------------------------------

Configuration: ``root_ways = 3`` at ``2*IGB``, ``region_ways = 6``
at ``IGB``, with a 2-way host-bridge interleave at ``IGB`` beneath
each root target. ``stride = 2*IGB / IGB = 2``.

Position layout::

   slot:    0     1     2     3     4     5
   target:  H0,a  H0,b  H1,a  H1,b  H2,a  H2,b

Positions {0,1} share root target H0, {2,3} share H1, {4,5} share
H2. ``cxl_calc_interleave_pos()`` produces these directly via the
mixed-gran root branch ``pos = pos + stride * parent_pos``: for the
endpoint at H1 inner-slot ``b`` (HB-position 1), the recurrence
yields ``pos = 1 + 2 * 1 = 3``.

The 3-way root consumes zero HPA selector bits; the host-bridge
2-way interleave contributes the region's only selector bit. The
span identity ``6 * IGB == 3 * 2*IGB`` holds.

Example: 8-way mixed-gran region on a 4-way power-of-2 root
-----------------------------------------------------------

Configuration: ``root_ways = 4``, ``root_gran = 4096``,
``region_ways = 8``, ``region_gran = 2048``, with a 2-way
host-bridge interleave at 2KB beneath each root target.
``stride = 4096 / 2048 = 2``.

Position layout (one root cycle covers 16KB)::

   HPA byte:  0     2KB   4KB   6KB   8KB   10KB  12KB  14KB
   position:  0     1     2     3     4     5     6     7
   target:    H0,a  H0,b  H1,a  H1,b  H2,a  H2,b  H3,a  H3,b

Positions {0,1} share host bridge H0, {2,3} share H1, and so on.
For the endpoint at H2 inner-slot ``b`` (HB-position 1), the
recurrence yields ``pos = 1 + 2 * 2 = 5``.

A same-granularity 8-way region on the same root would use the
standard multiplicative recurrence at every level. The additive
root branch is what places endpoints correctly when the root sits
at outer HPA bits and switches beneath it interleave at finer
granularity.


Linux Driver Implementation
===========================

Region geometry validation lives in ``drivers/cxl/core/region.c``.
The helpers and enforcement points below implement the rules
described in the preceding sections.

``get_selector(ways, gran)``
  Each interleave level consumes a contiguous range of HPA bits to
  pick its target. ``get_selector()`` models that range as a bitmask:
  ``(ways - 1) * gran`` for power-of-2 interleaves. A strict 3-way
  interleave consumes no HPA bits and the helper returns zero;
  6-way and 12-way are folded to expose only their power-of-2
  component.

  Kernel-doc: :c:func:`get_selector`.

``root_pos_stride(cxlr)``
  When ``region_gran < root_gran``, each root target owns more than
  one region position. The number of positions packed under one root
  target is ``root_gran / region_gran``: the root advances by
  ``root_gran`` bytes per target, while region positions advance by
  ``region_gran`` bytes. Same-granularity regions have a stride of
  one because the root advances on every region position.

  ``root_pos_stride()`` returns this ratio for the position walk and
  the peer-distance computation. The same value applies to 3-way
  family roots: in value space they advance by ``root_gran`` per
  target whether or not they consume HPA selector bits.

  Kernel-doc: :c:func:`root_pos_stride`.

``is_ig_allowed(cxlrd, ig)``
  The region granularity must not exceed the root granularity.
  ``region_gran > root_gran`` would place the root selector inside a
  single region stride, leaving the root no address bits to choose
  among its targets. ``region_gran <= root_gran`` is required for
  every legal layout.

  ``is_ig_allowed()`` enforces that geometric rule. It returns true
  when the root is not interleaving, or when ``ig`` does not exceed
  the root granularity. Endpoint capability (the IG groups a memdev
  advertises) is checked separately at attach time via
  ``check_interleave_cap()``.

  Kernel-doc: :c:func:`is_ig_allowed`.

``cxl_port_setup_targets()``
  This is the per-port enforcement point for the multi-level rule.
  For each level between the current port and the root the function:

  * Rejects overlap. Two levels that share a selector bit fail the
    disjoint requirement.
  * Rejects escapes. The accumulated selector must fit within the
    region selector. A level whose claimed bits land outside the
    region selector fails the containment requirement.
  * Derives the granularity to program at the current port for
    user-created regions. Each interleaving level claims the lowest
    selector bit still available within the region selector;
    passthrough levels consume no selector bit and inherit a scaled
    granularity from their ancestors.

  Auto regions reuse the same selector checks but trust the
  granularity firmware already programmed at each level.

``cxl_region_attach()``
  Pre-flight gates that run once per region attach, before the
  per-port selector walk.

  * Auto regions bypass the sysfs granularity gate, so
    ``is_ig_allowed()`` is applied here to give them the same
    geometric check as user-created regions.
  * 3-way-family roots require an explicit span check. A strict
    3-way root consumes no HPA selector bits, so selector containment
    cannot prove that the root and region cover the same address
    span. The span identity
    ``region_ways * region_gran == root_ways * root_gran`` restores
    that constraint. Power-of-2 roots get the equivalent constraint
    from selector containment and are not affected by this check.
    See `Span Identity for 3-Way-Family Roots`_ for the full
    derivation.

Position calculations use ``root_pos_stride()`` so that both
same-granularity and mixed-granularity regions produce the correct
endpoint ordering. The position walk itself lives in
``cxl_calc_interleave_pos()``.

Kernel-doc: :c:func:`cxl_calc_interleave_pos`.


Auto vs User-Created Regions
============================

**Auto regions** are reconstructed at boot from the decoder values
firmware programmed. The driver reads each decoder's ``ways`` and
``granularity``, runs them through the selector walk, and accepts
the configuration if it passes the disjoint and containment checks
(plus the span identity for 3-way roots). Firmware is the source of
truth for the granularity at each level.

**User-created regions** specify ``interleave_ways`` and
``interleave_granularity`` through sysfs; the driver derives each
intermediate decoder's granularity. The sysfs path runs
``is_ig_allowed()`` at granularity-write time, ``set_interleave_ways()``
at ways-write time, and the full selector walk when the region is
attached and committed.

Both paths converge on the same geometry validation rules.
Differences between the paths are limited to how region parameters
are obtained and when they are validated.

* Whether each intermediate decoder's granularity is read from
  hardware (auto) or derived (user).
* Whether the membership of the region is established by walking
  firmware-programmed decoders (auto) or by user writes to ``targetN``
  attributes (user).
