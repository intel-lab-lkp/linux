.. SPDX-License-Identifier: GPL-2.0

====================
Device Memory Buffer
====================

A device that offers ``VIRTIO_F_DMB`` presents one shared memory region,
the Device Memory Buffer, that it owns.  When the feature is negotiated
the region holds that device's virtqueues and the buffers its descriptors
reference, and every address the driver places in a virtqueue is an
offset from the start of the region rather than a physical or a bus
address.

Because each device has its own region, devices do not contend for one
shared bounce buffer, and the implementation of one device can be torn down
without touching the memory another device is using.  A consequence is
confinement of the virtqueue data path: to process virtqueues the device
can only reach memory the driver has published in the region.  It says
nothing about other shared memory regions the device exposes, or about
DMA the device performs outside a virtqueue.

.. note::

   ``VIRTIO_F_DMB`` is a specification proposal before the virtio
   Technical Committee, not a ratified feature:

     https://lore.kernel.org/virtio-comment/20260804161202.38619-1-graf@amazon.com/

   The feature bit number and the PCI register the driver reads the
   ``shmid`` from are both provisional, pending allocation by that
   committee, and the definitions of both in the uapi headers say so.  A
   device implementing this must expect either to change.

What lives in the region
========================

For every virtqueue of the device: its Descriptor Area, Driver Area and
Device Area.  For every buffer made available to the device: the buffer
itself.  With ``VIRTIO_F_INDIRECT_DESC`` also negotiated: every indirect
descriptor table, and the buffers the descriptors in it name.

Not in the region, and not changed by this feature: available and used
buffer notifications, configuration change notifications, the
configuration space, and everything else the transport conveys.  The
memory ordering requirements of the three virtqueue areas are unchanged.
What did change is that the driver accepts ``VIRTIO_F_DMB`` only together
with ``VIRTIO_F_ORDER_PLATFORM`` where the device offers it, because the
areas are now in memory the device supplies rather than memory the driver
allocated.

What the device must guarantee
==============================

* It must not offer ``VIRTIO_F_DMB`` unless the transport defines both a
  shared memory region discovery mechanism and a way to report which
  region is the Device Memory Buffer.  It must also offer
  ``VIRTIO_F_ACCESS_PLATFORM``.  Of the transports Linux implements only
  PCI defines the second, and only PCI accepts the feature bit at all: on
  every other transport it is cleared during transport feature negotiation
  and never reaches the device's driver.

* It must not offer ``VIRTIO_F_DMB`` unless the platform permits the
  driver to access the region as memory shared with the device, and unless
  a write by either side becomes visible to the other without any cache
  maintenance by the driver.  It must offer ``VIRTIO_F_ORDER_PLATFORM`` if
  it needs the driver to use barriers suitable for a device described by
  the platform, and the driver then accepts ``VIRTIO_F_DMB`` only together
  with it.

* It must expose exactly one shared memory region for the Device Memory
  Buffer, and report that region's ``shmid`` through the
  transport-specific mechanism.  On the PCI transport that is the
  ``dmb_shm_id`` field of the common configuration structure, whose value
  is confined to 8 bits because a ``virtio_pci_cap`` id is 8 bits wide,
  and a device offering the feature must present a common configuration
  structure long enough to contain the field.

* It must not report a ``dmb_shm_id`` equal to any ``shmid`` its device
  type defines.  The driver looks up only the region the device points it
  at, but the device-type driver bound to the same device looks up the ids
  its own specification reserves, so a collision has the two look up the
  same region.  The Device Memory Buffer claims the range first, because it
  is installed during feature negotiation and before the device-type driver
  probes, so a virtio-fs device reporting ``dmb_shm_id`` 0 fails to probe
  rather than sharing the region, and the failure names neither the feature
  nor the collision.

* It must interpret every address the driver supplies in a virtqueue as
  an offset from the start of the region, and must not interpret any of
  them as a physical or bus address.  That covers the three area
  addresses, the ``addr`` field of every descriptor, the address of every
  indirect descriptor table, and the ``addr`` field of every descriptor
  inside one.

* Before accessing anything a driver-supplied offset refers to, it must
  check that the offset together with the length of that structure lies
  within the region and does not overflow.  If the check fails it must
  not perform the access, and must set the ``DEVICE_NEEDS_RESET`` device
  status bit.

* It must not access memory outside the region in order to process
  virtqueues.

* A device that cannot function at all without a Device Memory Buffer
  must refuse ``FEATURES_OK`` when the driver has not accepted
  ``VIRTIO_F_DMB``.  Nothing else can catch that case: a driver that did
  not accept the feature proceeds with ordinary host-memory addresses,
  which such a device cannot reach, and it has no way to know that.

Exposing other shared memory regions alongside the Device Memory Buffer
is allowed; the driver looks up only the reported ``shmid`` and ignores
every other region.

The proposal does not state two of the obligations above: the
``dmb_shm_id`` collision rule, and the ``FEATURES_OK`` refusal by a
device that cannot work without a region.  Both follow from what the
driver does rather than from the standard, and the driver cannot check
either, so treat them as requirements anyway.

Coherency, and what this driver assumes
=======================================

The driver maps the region as ordinary cacheable memory shared with the
device and uses plain loads and stores on it, including for the virtqueue
areas themselves.  It asks for a mapping that does not apply the guest's
own memory encryption, because the device has to be able to read what the
driver writes there; that is what lets a guest whose memory is encrypted
use the feature at all.

Plain loads and stores are only correct if the region is coherent with
CPU caches as seen by the device, which is satisfied by a device whose
region is host memory.  A device whose region is not host memory must
additionally offer ``VIRTIO_F_ORDER_PLATFORM``: without it the ring's
barriers are the inner-shareable ones, which do not order accesses as
seen by a device outside that domain, and the available ring and the
notification would be reordered silently.

The driver accepts ``VIRTIO_F_DMB`` only if it also accepts
``VIRTIO_F_ACCESS_PLATFORM``, and, where the device offers
``VIRTIO_F_ORDER_PLATFORM``, only if it accepts that as well.  What it
cannot do is refuse the feature when ``VIRTIO_F_ORDER_PLATFORM`` is simply
absent from the offer: which barriers are needed depends on where the
region's memory is, the transport reports a ``shmid``, a base and a length
and nothing about the memory behind them, and refusing unconditionally
would reject the emulated device whose region is ordinary host memory and
for which the inner-shareable barriers are correct.  So a device whose
region needs the platform's barriers has to say so by offering the
feature, and one that does not misbehaves in a way that looks like a ring
bug.

The proposal states both requirements, and states the coupling the driver
implements.

A device whose region is not coherent with CPU caches at all is out of
scope here.  Supporting one would need two things that do not exist: a
way for the device to declare the memory type of its region, and explicit
flush points in the virtqueue code.  Mapping the region write-combining
instead is not a substitute, because on some architectures draining a
write-combining buffer needs an explicit barrier that virtio's ring
barriers do not issue, which would break available-ring and notification
ordering silently.

Sizing the region
=================

The region has to hold everything at once: every virtqueue's virtqueue
areas, and every buffer in flight.  Allocation is page-granular, so a
device sizing its region counts pages and not bytes.

The pool does not start at the start of the region.  The driver places it
at the first page-aligned address after the region base, so the bytes
ahead of it are lost: a whole page where the region base is page-aligned,
and the leading partial page, which was never usable, where it is not.  A
device must count that in what it offers rather than in what it expects to
be used, and since it cannot know the guest's ``PAGE_SIZE`` it cannot know
which of the two cases it is in, so it has to budget a whole page.  That
page is a page of the guest's ``PAGE_SIZE``, not of the device's own
granularity, and like every other figure here it has to be sized for the
largest page size the device expects a driver to run with.

Offset zero is never published, and that is why the page goes.  The
proposal reserves it: it is not the address of any structure the driver
places in the region, and a device may treat an offset of zero as an
error.  Device implementations would not accept it in any case: a device
that has ever driven a virtqueue uses a queue address of zero as its
"this queue was never programmed" sentinel, and QEMU's generic virtio
core and its vhost path both do, so a queue published at offset zero is
dropped silently, with no error reported anywhere and no way for the
driver to tell.  The driver keeps the value out of its pool rather than
rely on being an exception to that.

Which pages is the guest's business, and the device cannot find out.
Every count and every byte figure below is a multiple of the guest's
``PAGE_SIZE``, and the figures given are for a 4 KiB page; a guest with
64 KiB pages needs up to sixteen times the bytes for the same queue depth
and the same traffic.  So a device must size its region for the largest
page size it expects a driver to run with, or state the page size it was
sized for.

One buffer costs ``ceil(len / PAGE_SIZE)`` slots per scatterlist entry::

  slots(request) = sum over sg entries of ceil(len / PAGE_SIZE)
                 + ceil(nr_sg * 16 / PAGE_SIZE)   [1]

  [1] with VIRTIO_F_INDIRECT_DESC and for a chain of more than one entry,
      for the indirect descriptor table; one slot for up to 256 entries

The term that surprises people is neither the payload nor the indirect
table: it is the small scatterlist entries, because each costs a whole
page.  A virtio_blk read of 4 KiB, with ``VIRTIO_F_INDIRECT_DESC``
negotiated, spends one slot on the 16-byte header, one on the 1-byte
status, one on the indirect table and one on the data, so 16 KiB of
region carries 4 KiB of payload.  The 12 KiB of metadata is the same for
a 4 KiB request and for a 128 KiB one, so the amplification is worst for
small requests: 4x for that 4 KiB read, under 1.1x for a 128 KiB one.

How much a driver pays therefore depends on how it lays a request out,
and a virtio_net transmit costs one slot rather than the four above.
``VIRTIO_F_VERSION_1``, which a device offering this feature has to offer
anyway, lets virtio_net push its header into the skb headroom; a linear
skb then yields a single scatterlist entry, and
``virtqueue_use_indirect()`` wants more than one entry before it builds a
table.  One slot for the whole transmit, then: about 2.7x for a full-MTU
frame and about 57x for a 60-byte one, counting the pushed header as part
of what the slot carries.  An skb the driver cannot push into, or one
carrying fragments, pays a slot per entry and one more for the table.

How the pool is divided
-----------------------

The pool is not one flat range.  It is divided into *areas*, each with its
own lock, and a claim is served from one of them: the area belonging to the
CPU that asked, or the next area that has room.  This is the structure
``kernel/dma/swiotlb.c`` uses, and it exists so that mappings on different
CPUs do not serialise on a single lock, and so that the time one search
spends with interrupts disabled does not grow with the region.

.. note::

   In this document *area* on its own always means a pool area: one of the
   allocator's lock partitions, the quantity ``areas`` counts and
   ``area_pages`` sizes.  The Descriptor, Driver and Device Areas the
   specification names are always written out as *virtqueue areas*, and the
   pages one virtqueue's three areas occupy are ``ring_pages(num)``.

A pool area covers a power-of-two number of pages, never fewer than 512 or
one cacheline of the allocator's bitmap, whichever is larger, and never more
than 4096, and the count follows from that::

  area_pages = clamp(rounddown_pow2(pages / roundup_pow2(possible_cpus)),
                     max(512, L1_CACHE_BYTES * 8), 4096)
  areas      = ceil(pages / area_pages)

The last area is short unless ``area_pages`` divides ``pages``.  A region
small enough for one area to cover behaves exactly as a single lock over
the whole pool does, which includes every region up to 2 MiB on a 4 KiB
page, and more than that on a guest with few possible CPUs.
Both values are printed at probe time, so a device implementer can read
back what a guest derived from the region it offered.

A device cannot influence the count and should not try to: it is derived
from the guest's page size, cacheline size and possible-CPU count, none of
which the device knows.  What it costs the guest is one cacheline per pool
area, against one bit of bitmap and one slot record of ``sizeof(phys_addr_t)
+ 8`` bytes for every page of pool.  On a 512 MiB region on a guest with
sixty-five or more possible CPUs, where the pool comes to 256 areas of 512
pages, that is 16 KiB of areas against 2 MiB of slot records.

No single buffer mapping may exceed an eighth of the pool or half of one
pool area, whichever is smaller, rounded down to a page; and never less
than one page, which is the floor that governs a region at the four-page
minimum, where an eighth rounds down to nothing.  That is what
``max_mapping_size`` reports, through ``virtio_max_dma_size()``.  Only
virtio_blk consults it, as its segment-size limit; on a driver that does
not, an over-cap mapping is refused rather than split, and that refusal is
permanent rather than back-pressure.  The eighth bounds the capacity one
mapping can deny the rest of the device.  The half-area is not a choice: an
allocation has to lie inside one pool area so that a single lock covers it,
and half rather than all of an area is headroom, because a request the size
of an area could only ever be satisfied by a completely empty one.  Which of
the two governs follows from the geometry rather than from the region size
alone, because ``area_pages`` is itself derived from the possible-CPU count:
the half-area becomes the smaller only once ``area_pages`` has fallen below a
quarter of ``pages``.  On a 16 MiB region with three or more possible CPUs it
has, and the cap is 1048576 bytes rather than the 2093056 an eighth alone
would give; on a one- or two-CPU guest the eighth still governs a 16 MiB
region.  Virtqueue areas are allocated from the region too and are subject to
neither cap, though an ``alloc()`` still has to fit inside one pool area.  A
split ring costs one allocation for all three virtqueue areas together, of
``vring_size(num, align)`` bytes, which at the cache-line alignment the
PCI transport passes is one slot at a queue depth of 128, two at 256,
seven at 1024 and twenty-seven at 4096.  A packed ring costs three
separate allocations, the descriptor ring and two event structures, and
the two event structures are four bytes each: a packed queue therefore
always spends two whole pages on eight bytes.

So a device must offer at least::

  region_bytes >= PAGE_SIZE
                + sum over virtqueues of
                    [ ring_pages(num) + num * slots(max_request) ]
                    * PAGE_SIZE

  where the leading PAGE_SIZE covers the bytes ahead of the pool, and is
  a whole page because a device cannot know the guest's page size and so
  cannot know whether its region base is aligned to one,
  slots(max_request) is the largest slots(request) value from the
  formula above the driver can produce for one request on that queue,
  and ring_pages(num) is the pages one virtqueue's Descriptor, Driver and
  Device Areas occupy: ceil(vring_size(num, align) / PAGE_SIZE) for a
  split ring, and ceil(num * 16 / PAGE_SIZE) + 2 for a packed one

A device that also offers an administration virtqueue pays for it out of
the same region: its virtqueue areas are allocated through the same path,
and one
administration command occupies several slots more.

A driver that resizes a queue needs the region to hold both copies of its
virtqueue areas at once.  ``virtqueue_resize()``, which ``VIRTIO_F_RING_RESET``
makes reachable and which ``ethtool -G`` reaches on virtio_net, allocates
the new virtqueue areas before it frees the old, so on a region sized for
one copy
it is the new allocation that fails.  What happens then differs by ring
layout, and only one of the two is a refusal.  A packed ring cannot be
made smaller, so the resize fails and the queue keeps the depth it had.
A split ring on the PCI transport is created with ``may_reduce_num`` set,
so it halves the requested depth until the virtqueue areas fit and then
reports success at that smaller depth: a driver asking for a deeper queue
can get a shallower one instead, and the depth the queue goes on to report
is the only thing that says so.  The halving stops once they fit in a single
page, which at 4 KiB pages and cache-line alignment is a depth of 128, so a
region with no free page at all fails the resize as a packed ring would.

A device must offer a region of at least four usable pages, which is five
pages of region where the region base is page-aligned.  The driver refuses
a smaller one and sets the ``FAILED`` device status bit.  That floor is
derived from one minimally-sized virtqueue and one buffer in
flight against it, so it is a lower bound on any working configuration
and not a size a device with several queues can sit on.  Above that floor
a device that offers less than the formula asks for still probes, and the
shortfall appears as reduced effective queue depth: the ring reports a
mapping failure and the driver applies back-pressure.  One case is not
back-pressure: a mapping longer than ``max_mapping_size`` is refused however
empty the pool is, and retrying it never succeeds.  The allocator is next fit
within one pool area, from a hint that advances past each claim and rewinds to
each release, beginning in the area belonging to the running CPU and then
trying each other area in turn, and nothing is moved, so a region that meets
the formula can still fail an individual mapping to fragmentation.  Either is
legal, and either should be a deliberate choice rather than a surprise.

A region too small even for the virtqueue areas is a different case.  A
split ring halves the queue size and retries, so such a region can still
produce a working queue shorter than the device asked for, down to the
depth whose virtqueue areas fit in one page and no further; a packed ring
cannot be made smaller at all, so queue creation fails.  The driver logs the
size of the virtqueue area that did not fit and how many pages the region
has: at
warning level for an attempt the ring did not mark as retryable, and at
debug level for the ones it did, which for a packed ring is all of them.

Sizing the region for several virtqueues
----------------------------------------

The formula above is per virtqueue, and the sum over a multi-queue device
is what matters.  A driver creates every virtqueue the device offers, not
the ones it goes on to use: ``virtnet_find_vqs()`` creates
``2 * max_queue_pairs`` virtqueues plus a control virtqueue, whatever
``curr_queue_pairs`` ends up being.  So::

  V         = 2 * Q + C          Q = max_queue_pairs, C = 1 with
                                 VIRTIO_NET_F_CTRL_VQ
  rings     = 2 * Q * ring_pages(num) + C * ring_pages(num_ctrl)
                                 the table below takes num_ctrl = num,
                                 which over-counts a device giving its
                                 control virtqueue a shallower ring
  rx_fill   = Q * num * B_rx     B_rx = 1 with VIRTIO_NET_F_MRG_RXBUF
                                 (or small buffers), MAX_SKB_FRAGS + 3
                                 without it
  tx_min    = Q * (MAX_SKB_FRAGS + 3)   one worst-case segmented packet
                                        per transmit queue, each of whose
                                        scatterlist entries is assumed to
                                        fit in one page
  ctrl      = 5                  header, status, up to two data entries and
                                 the indirect table they take, since
                                 virtqueue_use_indirect() builds one for
                                 more than a single entry; each entry is
                                 assumed to fit in one page

  floor_slots = rings + Q * B_rx + tx_min + ctrl   multi-queue works: one
                                                   receive buffer per queue
  work_slots  = rings + rx_fill + tx_min + ctrl    receive rings full

Every term above assumes ``VIRTIO_F_INDIRECT_DESC``, which is what the
``+ 3`` and the ``ctrl`` table account for.  Without it a chain occupies one
descriptor per entry instead of one, so a queue of depth ``num`` posts fewer
buffers and ``rx_fill`` falls rather than rises.

``try_fill_recv()`` fills a receive queue until it has no free descriptors,
so ``rx_fill`` and not ``floor_slots`` is the working figure.  For a split
ring, x86_64, 4 KiB pages, ``MAX_SKB_FRAGS`` 17, mergeable receive buffers
and ``C = 1``:

=====  =====  =======  =========  ============  ==========
Q      num    rings    rx_fill    work_slots    work MiB
=====  =====  =======  =========  ============  ==========
1      256    6        256        287           1.1
4      256    18       1024       1127          4.4
8      256    34       2048       2247          8.8
8      1024   119      8192       8476          33.1
16     1024   231      16384      16940         66.2
32     1024   455      32768      33868         132.3
64     1024   903      65536      67724         264.5
=====  =====  =======  =========  ============  ==========

Two things to take from it.  ``rx_fill`` dominates: it is 65536 slots of the
67724 sixty-four queue pairs come to, so sizing a region for multiple queues
is "how many receive buffers will be posted" to within three and a quarter
percent, and the virtqueue rings are the smaller part of what is left -- 903
slots against ``tx_min``'s 1280.  And the pool areas of the previous
subsection do not appear at all, because they cost guest memory rather than
region pages.

The receive buffer mode is a twenty-fold multiplier the device cannot
predict.  A driver that negotiates any of ``VIRTIO_NET_F_GUEST_TSO4``,
``VIRTIO_NET_F_GUEST_TSO6``, ``VIRTIO_NET_F_GUEST_ECN`` or
``VIRTIO_NET_F_GUEST_UFO`` *without* ``VIRTIO_NET_F_MRG_RXBUF`` uses big
receive buffers, which cost ``MAX_SKB_FRAGS + 3`` slots each instead of
one: the eight-queue, depth-256 row above goes from 2247 slots to 41159,
which is 160.8 MiB instead of 8.8.  A device offering guest segmentation
offload without ``MRG_RXBUF`` must size for that.  Offering ``MRG_RXBUF``
is the better answer.

An undersized multi-queue region fails in two ways, and both are
properties of the network driver's existing behaviour rather than of the
region.

First, cross-queue starvation.  ``virtnet_open()`` pre-fills the receive
queues in index order and discards the result, so queue 0 takes what it
needs before queue 1 asks.  A region that cannot hold every queue's fill
leaves the later queues with no buffers at all; the device's receive
steering then drops whatever it sends to them, and transmit fails on every
queue with ``tx_fifo_errors``, ``tx_dropped`` and a rate-limited
``Unexpected TXQ`` message.  Pool areas do not help: at
``virtnet_open()`` every fill runs on whichever CPU brought the link up, so
they all share one home area.  Areas give preference, never reservation.

Second, a receive queue that cannot refill spends softirq time without
making progress.  ``try_fill_recv()`` reports failure, ``virtnet_receive()``
returns the full budget to force a repoll, and ``virtnet_poll()`` therefore
never completes NAPI.  There is no delayed worker behind that: commit
1e7b90aa7988 ("virtio-net: remove unused delayed refill worker") removed it
deliberately, "since we switched to retry refilling receive buffer in NAPI
poll instead of delayed worker".  The repoll always recovers once capacity
is released, and it burns a CPU until then.

What the driver does
====================

Feature negotiation comes first.  ``VIRTIO_F_DMB`` is accepted only when
the same offer carries ``VIRTIO_F_ACCESS_PLATFORM`` and
``VIRTIO_F_VERSION_1``, and only when the kernel was built with
``CONFIG_VIRTIO_DMB``.  Once the device has
confirmed ``FEATURES_OK``, and not before, the driver reads the
``shmid``, locates the region through the ordinary shared memory
enumeration, and maps it.  That happens during feature negotiation and so
before the device-type driver's own probe, and it records a claim on the
range, which appears in ``/proc/iomem``.  The claim is advisory: a range
something else already owns is mapped anyway and nothing fails.  If it
cannot locate the region it sets the ``FAILED`` device status bit: it has
already committed to the feature by accepting it, so refusing at that point
is not available to it.

The driver then places every virtqueue area, indirect table and buffer
inside the
region and publishes offsets from the region start.  Allocation is
page-granular, which is what makes each absolute address satisfy the
alignment the virtqueue layout in use requires, since the region base
carries no alignment guarantee of its own.  Buffers that were not
allocated from the region are copied into it, and copied back out on
unmap for any mapping the device may have written, so a driver needs no
change to its data path to work with such a device.

That copy in happens for every mapping, in either direction and whether
or not the caller asked for the CPU sync to be skipped, because a device
that writes only part of a buffer has to leave the rest of the caller's
bytes as they were.  It covers the length of that mapping and nothing
beyond it, so where a buffer does not end on a page boundary the rest of
its last page still holds what that page held before: an earlier mapping
of this device, a freed virtqueue area, or what the device itself left
there.  A device is given the length of every buffer it is offered, and
nothing outside that length is part of the buffer.

A driver bound to such a device must delete its virtqueues before it
creates them again across a suspend or a reset.  When the region is
unchanged the allocator is kept as it was, so a virtqueue that survived
still holds slots the pool has recorded as in use, and a second
``find_vqs()`` claims fresh slots alongside them that nothing will
release.

The region is located after every feature negotiation, so its lifetime
is the lifetime of the negotiation and not of the device.  A negotiation
that reports the same shmid, base and usable length as last time keeps
the region's kernel mapping that is already there, so an offset
published before it stays valid.  A negotiation that reports a different
region replaces that mapping only when no virtqueue is live; nothing the
device left in the old one is reachable afterwards.  A different region
under live virtqueues is refused and the device is marked ``FAILED``, so
a device must not move its region, or resize it by a whole page or more,
across a suspend or a reset it expects the driver's virtqueues to
survive.  Either way, a device must not treat anything it left in the
region as still valid once the driver has reset it.

Keeping the existing kernel mapping is also the whole of what happens
across hibernation, and that carries a limitation worth stating.  That
mapping and the pool that describes it are ordinary kernel memory, so
they are part of the hibernation image, and on the way back up the
driver adopts them rather than establishing it again.  Whether the
kernel address it sits at still reaches the region depends on the kernel
page tables being restored from the image along with everything else,
which is the same thing every driver carrying a ``memremap()`` or
``ioremap()`` across hibernation relies on, and nothing here checks it.
Establishing it again instead is not available while virtqueues are
live: a virtqueue holds the kernel addresses of its own virtqueue areas,
which are addresses inside it, and this code cannot rewrite them.
Tearing the region down at freeze time in order to make that safe is the
one thing it must not do, because a driver with no freeze callback
cannot be torn down, and a device that refuses to be frozen refuses
suspend for the whole system.

The region's length bounds how much virtqueue data can be in flight at
once.  Running out of room is therefore an ordinary condition and not an
error: a mapping fails and the driver applies the back-pressure it
already has for a full queue.  The errno is whichever the ring already
reports for a failed mapping, which is ``-ENOMEM`` from a split ring and
from a packed indirect table, and ``-EIO`` from a packed ring using
direct descriptors.  A driver must therefore treat any error from
``virtqueue_add_*()`` as back-pressure, and must not treat a particular
errno as the only indication of exhaustion.

Under this feature the virtqueue data path calls no DMA mapping function
at all, so no bus address is ever passed to the device and the
``VIRTIO_F_ACCESS_PLATFORM`` obligation to translate one has nothing to
act on.  The IOMMU is neither disabled nor programmed by this path.  DMA
the device performs that does not go through a virtqueue, such as MSI-X
writes, is programmed by the transport as usual and is unaffected.

``virtqueue_dma_dev()`` returns NULL for such a device, which every
driver bound to it inherits whether or not it asked for anything.  A
driver that maps buffers itself loses that ability, and in virtio_net the
consequences are worth stating precisely because they are narrower than
"page_pool is unavailable".  ``virtnet_create_page_pools()`` branches on
``virtqueue_dma_dev()`` and still creates the pool, without
``PP_FLAG_DMA_MAP`` and ``PP_FLAG_DMA_SYNC_DEV``: allocation and recycling
both keep working, and only the mapping the pool would have done is lost,
so the ring maps each buffer instead.  Plain XDP works, being gated on
headroom rather than on a mapping device.  ``AF_XDP`` does not:
``virtnet_xsk_pool_enable()`` refuses outright without a mapping device.

The cost is that a receive buffer is mapped and copied on every post and
unmapped and copied on every completion, where a driver that could map for
itself would map once and recycle.  Nothing here removes that, and the
reasons are structural rather than a matter of scheduling.  Making
``virtqueue_dma_dev()`` return the parent device would be untrue: page_pool
would call ``dma_map_page()`` on it and produce addresses the device cannot
use, since a device using this feature reads offsets into its region.  A
region-backed page_pool would have to allocate its pages inside the region,
which is a larger change than this.  And eliding the copy on a receive
mapping needs ``DMA_ATTR_SKIP_CPU_SYNC`` both honoured on the map side and
produced by a driver, and no producer of it exists in the ring or in any
driver today.  VDUSE already imposes the same downgrade for the same
reason, so drivers handle it, but it is a capability the device removes
from the driver rather than one the driver declines.

Observing a region
==================

A device that negotiates the feature reports what it got, the first time
the driver installs the region and again only if the device later reports
a different one::

  virtio_net virtio5: device memory buffer 2 at 0x00000000c0000000, 4095 usable pages in 8 areas of 512 pages

That names the ``shmid`` the device reported, where the region turned out
to be, how many pages the allocator can hand out, which is the region
length less the bytes ahead of the pool and less any trailing partial
page, and the pool-area geometry the guest derived.  The count multiplied
by the page size is therefore smaller than
the length the transport reported for the region, and a device sizing
itself against the count rather than the length is the one that matches.
The eight areas of 512 pages in the example are what 4095 pages come to on
a guest with three or more possible CPUs; a guest with one or two derives
fewer, larger areas from the same region.
The claimed range also appears in ``/proc/iomem`` as
``virtio-dmb``, but only where the claim was granted, and the ``shmid``
appears nowhere else at all.

Notes for kernel code
=====================

A ``dma_addr_t`` belonging to a device using a Device Memory Buffer is a
byte offset into that region.  It is not a DMA address, it is not valid
for any DMA API call, and no code outside the device's
``virtio_map_ops`` may treat it as either.  It is never zero: the pool
starts after the first byte of the region, so that no address handed to
the device can be mistaken by it for a queue it was never given, and the
handle validation rejects zero along with every other offset below the
pool.

The contents of the region are readable and writable by the device at
all times, including the descriptor table, the available ring and every
buffer.  No value read back from the region may be used to compute a
kernel address, a length or an index without being validated first.  In
particular, a header the driver has already validated may have been
rewritten by the time it is read a second time.
