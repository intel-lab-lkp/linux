.. SPDX-License-Identifier: GPL-2.0

====================
DAX Driver Operation
====================
The `Direct Access Device` driver was originally designed to provide a
memory-like access mechanism to memory-like block-devices.  It was
extended to support CXL Memory Devices, which provide user-configured
memory devices.

The CXL subsystem depends on the DAX subsystem to either:

- Generate a file-like interface to userland via :code:`/dev/daxN.Y`, or
- Engage the memory-hotplug interface to add CXL memory to page allocator.

The DAX subsystem exposes this ability through the `cxl_dax_region` driver.
A `dax_region` provides the translation between a CXL `memory_region` and
a `DAX Device`.

DAX Device
==========
A `DAX Device` is a file-like interface exposed in :code:`/dev/daxN.Y`. A
memory region exposed via dax device can be accessed via userland software
via the :code:`mmap()` system-call.  The result is direct mappings to the
CXL capacity in the task's page tables.

Users wishing to manually handle allocation of CXL memory should use this
interface.

Dynamic Capacity (DC) Regions
=============================
A region backed by a CXL `Dynamic Capacity Device (DCD)` is a `DC region`:
its HPA window is fixed at probe time, but the DPA capacity that fills the
window arrives and departs at runtime as the device offers and reclaims
`extents`.  DC regions are distinguished from static regions by the
:code:`IORESOURCE_DAX_DCD` flag on the :code:`dax_region`.

For the CXL-side rules governing when an offered extent is accepted or a
release request is honoured, see :doc:`cxl-driver`.  This section covers
the DAX-side mapping between accepted extents and DAX devices.

The Extent Layering Model
-------------------------
Four objects sit between the wire-level CXL extent and the
user-visible DAX device.  Understanding the cardinality between them
is the key to the DC-region model.

::

    device extents     dc_extent           dax_resource         DAX device
    (CXL device)       (CXL core)          (DAX bus)            (/dev/daxN.Y)
    -------------      -------------       -------------        ------------
    e1 ─┐                ┌─► dc_e1 ──►     res_1 (seq=1) ──┐
    e2 ─┼─── tag A ──►   ┼─► dc_e2 ──►     res_2 (seq=2) ──┼──►  daxN.0
    e3 ─┘                └─► dc_e3 ──►     res_3 (seq=3) ──┘     (claimed by tag A,
                                                                   size = Σ |e_i|)

    e4 ─── tag B ────►     dc_e4 ──►       res_4 (seq=1) ────►   daxN.1

    e5 ─── null tag ─►     dc_e5 ──►       res_5 (seq=0) ────►   daxN.2
    e6 ─── null tag ─►     dc_e6 ──►       res_6 (seq=0) ────►   daxN.3

The CXL core groups extents sharing a non-null tag into a single
:code:`cxl_dc_tag_group` (internal-only, no sysfs identity), but each
member extent stays a distinct :code:`dc_extent` with its own HPA
range.  The DAX bridge creates one :code:`dax_resource` per
:code:`dc_extent`, and userspace claims a DAX device by writing the
tag's UUID to the seed device's :code:`uuid` attribute, which carves
every matching :code:`dax_resource` (in :code:`seq_num` order) into
the device's :code:`ranges[]` array.

`Device extent`
  The unit the CXL device delivers over the mailbox: a
  :code:`(DPA, length, tag, shared_extn_seq)` tuple inside an
  Add-Capacity event.  The tag is either a non-null UUID (a
  `tagged allocation`) or the null UUID (`untagged`).

:code:`dc_extent`
  The CXL core's per-extent object, one per surviving device extent.
  Each :code:`dc_extent` is registered as its own :code:`extentX.Y`
  sysfs device under :code:`cxlr_dax->dev` and carries its own
  :code:`hpa_range` — there is no aggregated / bounding-box HPA
  range across siblings.  Members of one tag group point at a
  shared :code:`cxl_dc_tag_group` (which holds the UUID and a
  manual refcount on the surviving siblings) but otherwise exist as
  independent kernel objects.

  For a `non-null tag`, the host-wide tag-uniqueness gate
  (:doc:`cxl-driver`) guarantees there is at most one
  :code:`cxl_dc_tag_group` per UUID on the host, so the set of
  :code:`dc_extent`\ s sharing that UUID is a single allocation.

  For the `null tag` there is no cross-event identity — the spec is
  silent on aggregating untagged extents across Add-Capacity events.
  Each untagged device extent becomes its own :code:`dc_extent` in
  its own single-member tag group; two untagged extents delivered
  separately are two distinct allocations.

:code:`dax_resource`
  The DAX bus's per-extent view, one-to-one with :code:`dc_extent`.
  When the CXL DAX driver receives a :code:`DCD_ADD_CAPACITY`
  notification it iterates the tag group and calls
  :code:`dax_region_add_resource()` once per member, creating one
  :code:`dax_resource` per :code:`dc_extent`.  Each
  :code:`dax_resource` carries that member's HPA range, the tag
  UUID (copied from :code:`dc_extent->group->uuid`), and a 1..n
  :code:`seq_num` so :code:`uuid_claim_tagged` can carve the matched
  set into the device's :code:`ranges[]` array in the right order
  (see :code:`drivers/dax/bus.c`).

`DAX device` (:code:`/dev/daxN.Y`)
  Created by userspace claiming a set of :code:`dax_resource`\ s via
  the :code:`uuid` sysfs attribute.  Each DAX device corresponds to
  exactly one allocation:

  * A `tagged` DAX device is built from every :code:`dax_resource`
    carrying the tag — one per :code:`dc_extent` in the allocation
    — carved into the device's :code:`ranges[]` in :code:`seq_num`
    order.  Its size equals the sum of every member's size.
  * An `untagged` DAX device is built from one untagged
    :code:`dax_resource` and its size equals that one extent.

So the end-to-end rule is: **one tagged allocation = one
cxl_dc_tag_group = N dc_extents = N dax_resources = one DAX device
with N range entries**.  An untagged device extent becomes its own
:code:`dc_extent` / :code:`dax_resource` / single-range DAX device,
claimed one at a time.

Release follows the same layering in reverse.  When the CXL core
calls :code:`rm_tag_group()` (after the device asks for release and
the DAX layer consents), the DAX bridge collects every matching
:code:`dax_resource` and removes them as a set via
:code:`dax_region_rm_resources()`.  The removal is refuse-all-or-none
under :code:`dax_region_rwsem`: if any member is in use, the whole
group stays.  When removal commits, the HPA capacity returns to the
region's free pool and any DAX device that had claimed it is left
with no backing capacity.  Userspace tears the DAX device down via
:code:`daxctl destroy-device` (size=0, then write the device name to
the region's :code:`delete` attribute).

UUID-Based DAX Device Creation
------------------------------
A DAX device on a DC region is created by writing a UUID to the
seed device's :code:`uuid` attribute
(:code:`/sys/bus/dax/devices/daxN.Y/uuid`).  The seed starts at
size 0; writing :code:`uuid` is a `claim` operation that resolves
the layering above and populates the device:

* A `non-null UUID` claims `every` :code:`dax_resource` whose tag
  matches.  :code:`uuid_claim_tagged` (in
  :code:`drivers/dax/bus.c`) collects them, sorts by
  :code:`seq_num`, enforces the dense :code:`1..n` invariant, and
  carves each via :code:`__dev_dax_resize` in :code:`seq_num` order
  so the device's :code:`ranges[]` array is dense and ordered.
  The resulting DAX device represents exactly the tagged
  allocation: its size equals the sum of every member extent's
  size.

  The dense :code:`1..n` invariant is the unified rule the CXL
  side maintains for both sharable and non-sharable allocations
  (see :doc:`cxl-driver`); the match set has exactly one entry per
  :code:`dc_extent` in the tag group.

* The value :code:`"0"` is shorthand for the null UUID and claims
  exactly `one` untagged :code:`dax_resource`.  Untagged
  :code:`dax_resource`\ s correspond to independent untagged
  allocations; collapsing several into one device would aggregate
  unrelated capacity, so each :code:`uuid` write consumes a single
  untagged resource.

* A write that matches no :code:`dax_resource` returns
  :code:`-ENOENT` and the device remains at size 0.

* Writes to the :code:`uuid` attribute on non-DC regions return
  :code:`-EOPNOTSUPP`; the attribute itself is read-only (0444) on
  non-DC devices.

The device's size is determined entirely by the backing allocation:
users do not choose a size on DC regions.  Accordingly, the
:code:`size` attribute on a DC DAX device rejects grow requests
with :code:`-EOPNOTSUPP`.  Writing :code:`0` is still permitted and is
how :code:`daxctl destroy-device` returns each claimed extent to the
region's available pool before the device's name is written to the
region's :code:`delete` attribute.

Reads of :code:`uuid` report the tag identifying the capacity
backing the device:

* For a non-null-UUID-claimed DC DAX device, :code:`uuid` reads
  back the claimed UUID.
* For a DC DAX device claimed via :code:`"0"`, or for any
  non-DCD DAX device, :code:`uuid` reads :code:`0`.

See :code:`Documentation/ABI/testing/sysfs-bus-dax` for the
authoritative attribute contracts.

kmem conversion
===============
The :code:`dax_kmem` driver converts a `DAX Device` into a series of `hotplug
memory blocks` managed by :code:`kernel/memory-hotplug.c`.  This capacity
will be exposed to the kernel page allocator in the user-selected memory
zone.

The :code:`memmap_on_memory` setting (both global and DAX device local)
dictates where the kernell will allocate the :code:`struct folio` descriptors
for this memory will come from.  If :code:`memmap_on_memory` is set, memory
hotplug will set aside a portion of the memory block capacity to allocate
folios. If unset, the memory is allocated via a normal :code:`GFP_KERNEL`
allocation - and as a result will most likely land on the local NUM node of the
CPU executing the hotplug operation.
