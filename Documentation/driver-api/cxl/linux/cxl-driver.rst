.. SPDX-License-Identifier: GPL-2.0

====================
CXL Driver Operation
====================

The devices described in this section are present in ::

  /sys/bus/cxl/devices/
  /dev/cxl/

The :code:`cxl-cli` library, maintained as part of the NDTCL project, may
be used to script interactions with these devices.

Drivers
=======
The CXL driver is split into a number of drivers.

* cxl_core  - fundamental init interface and core object creation
* cxl_port  - initializes root and provides port enumeration interface.
* cxl_acpi  - initializes root decoders and interacts with ACPI data.
* cxl_p/mem - initializes memory devices
* cxl_pci   - uses cxl_port to enumerate the actual fabric hierarchy.

Driver Devices
==============
Here is an example from a single-socket system with 4 host bridges. Two host
bridges have a single memory device attached, and the devices are interleaved
into a single memory region. The memory region has been converted to dax. ::

  # ls /sys/bus/cxl/devices/
    dax_region0  decoder3.0  decoder6.0  mem0   port3
    decoder0.0   decoder4.0  decoder6.1  mem1   port4
    decoder1.0   decoder5.0  endpoint5   port1  region0
    decoder2.0   decoder5.1  endpoint6   port2  root0


.. kernel-render:: DOT
   :alt: Digraph of CXL fabric describing host-bridge interleaving
   :caption: Diagraph of CXL fabric with a host-bridge interleave memory region

   digraph foo {
     "root0" -> "port1";
     "root0" -> "port3";
     "root0" -> "decoder0.0";
     "port1" -> "endpoint5";
     "port3" -> "endpoint6";
     "port1" -> "decoder1.0";
     "port3" -> "decoder3.0";
     "endpoint5" -> "decoder5.0";
     "endpoint6" -> "decoder6.0";
     "decoder0.0" -> "region0";
     "decoder0.0" -> "decoder1.0";
     "decoder0.0" -> "decoder3.0";
     "decoder1.0" -> "decoder5.0";
     "decoder3.0" -> "decoder6.0";
     "decoder5.0" -> "region0";
     "decoder6.0" -> "region0";
     "region0" -> "dax_region0";
     "dax_region0" -> "dax0.0";
   }

For this section we'll explore the devices present in this configuration, but
we'll explore more configurations in-depth in example configurations below.

Base Devices
------------
Most devices in a CXL fabric are a `port` of some kind (because each
device mostly routes request from one device to the next, rather than
provide a direct service).

Root
~~~~
The `CXL Root` is logical object created by the `cxl_acpi` driver during
:code:`cxl_acpi_probe` - if the :code:`ACPI0017` `Compute Express Link
Root Object` Device Class is found.

The Root contains links to:

* `Host Bridge Ports` defined by CHBS in the :doc:`CEDT<../platform/acpi/cedt>`

* `Downstream Ports` typically connected to `Host Bridge Ports`.

* `Root Decoders` defined by CFMWS the :doc:`CEDT<../platform/acpi/cedt>`

::

  # ls /sys/bus/cxl/devices/root0
    decoder0.0          dport0  dport5    port2  subsystem
    decoders_committed  dport1  modalias  port3  uevent
    devtype             dport4  port1     port4  uport

  # cat /sys/bus/cxl/devices/root0/devtype
    cxl_port

  # cat port1/devtype
    cxl_port

  # cat decoder0.0/devtype
    cxl_decoder_root

The root is first `logical port` in the CXL fabric, as presented by the Linux
CXL driver.  The `CXL root` is a special type of `switch port`, in that it
only has downstream port connections.

Port
~~~~
A `port` object is better described as a `switch port`.  It may represent a
host bridge to the root or an actual switch port on a switch. A `switch port`
contains one or more decoders used to route memory requests downstream ports,
which may be connected to another `switch port` or an `endpoint port`.

::

  # ls /sys/bus/cxl/devices/port1
    decoder1.0          dport0    driver     parent_dport  uport
    decoders_committed  dport113  endpoint5  subsystem
    devtype             dport2    modalias   uevent

  # cat devtype
    cxl_port

  # cat decoder1.0/devtype
    cxl_decoder_switch

  # cat endpoint5/devtype
    cxl_port

CXL `Host Bridges` in the fabric are probed during :code:`cxl_acpi_probe` at
the time the `CXL Root` is probed.  The allows for the immediate logical
connection to between the root and host bridge.

* The root has a downstream port connection to a host bridge

* The host bridge has an upstream port connection to the root.

* The host bridge has one or more downstream port connections to switch
  or endpoint ports.

A `Host Bridge` is a special type of CXL `switch port`. It is explicitly
defined in the ACPI specification via `ACPI0016` ID.  `Host Bridge` ports
will be probed at `acpi_probe` time, while similar ports on an actual switch
will be probed later.  Otherwise, switch and host bridge ports look very
similar - the both contain switch decoders which route accesses between
upstream and downstream ports.

Endpoint
~~~~~~~~
An `endpoint` is a terminal port in the fabric.  This is a `logical device`,
and may be one of many `logical devices` presented by a memory device. It
is still considered a type of `port` in the fabric.

An `endpoint` contains `endpoint decoders` and the device's Coherent Device
Attribute Table (which describes the device's capabilities). ::

  # ls /sys/bus/cxl/devices/endpoint5
    CDAT        decoders_committed  modalias      uevent
    decoder5.0  devtype             parent_dport  uport
    decoder5.1  driver              subsystem

  # cat /sys/bus/cxl/devices/endpoint5/devtype
    cxl_port

  # cat /sys/bus/cxl/devices/endpoint5/decoder5.0/devtype
    cxl_decoder_endpoint


Memory Device (memdev)
~~~~~~~~~~~~~~~~~~~~~~
A `memdev` is probed and added by the `cxl_pci` driver in :code:`cxl_pci_probe`
and is managed by the `cxl_mem` driver. It primarily provides the `IOCTL`
interface to a memory device, via :code:`/dev/cxl/memN`, and exposes various
device configuration data. ::

  # ls /sys/bus/cxl/devices/mem0
    dev       firmware_version    payload_max  security   uevent
    driver    label_storage_size  pmem         serial
    firmware  numa_node           ram          subsystem

A Memory Device is a discrete base object that is not a port.  While the
physical device it belongs to may also host an `endpoint`, the relationship
between an `endpoint` and a `memdev` is not captured in sysfs.

DPA Partitions
~~~~~~~~~~~~~~
A memory device presents its capacity as one flat `Device Physical Address`
(DPA) space divided into `partitions`, which Linux lays out in a fixed
order::

  DPA 0                                                     end
  +---------------+---------------+---------------------------+
  |      ram      |     pmem      |       dynamic_ram_1       |
  +---------------+---------------+---------------------------+
  part[0]         part[1]         part[2]

Part of that order is required by the CXL specification and part of it is a
Linux choice.

The `ram` and `pmem` order is mandated.  CXL r4.0 section 8.2.10.9.2.1 "Get
Partition Info" (4100h), Table 8-310, mandates that volatile capacity starts
at DPA 0 and pmem starts at the DPA immediately following it.

Dynamic Capacity partitions only need to be 256MB aligned according to
CXL r4.0 section 8.2.10.9.9.1 "Get Dynamic Capacity Configuration"
(opcode 4800h), Table 8-347. So a device could leave a gap between ram/pmem
(static) capacity and its first DC partition, or between one DC partition
and the next.

Linux chooses not to support gaps between static and dynamic capacity: the
first DC partition must begin at the DPA immediately following static
capacity -- after pmem, after ram on a device with no pmem, or at DPA 0 on
a device with no static capacity at all.

Currently, only one dynamic partition is supported. A device may report up
to eight (CXL r4.0 Table 8-346); Linux configures the first as its
`dynamic_ram_1` partition. Gaps between DC partitions are not checked, since
only the first is used.

Port Relationships
~~~~~~~~~~~~~~~~~~
In our example described above, there are four host bridges attached to the
root, and two of the host bridges have one endpoint attached.

.. kernel-render:: DOT
   :alt: Digraph of CXL fabric describing host-bridge interleaving
   :caption: Diagraph of CXL fabric with a host-bridge interleave memory region

   digraph foo {
     "root0"    -> "port1";
     "root0"    -> "port2";
     "root0"    -> "port3";
     "root0"    -> "port4";
     "port1" -> "endpoint5";
     "port3" -> "endpoint6";
   }

Decoders
--------
A `Decoder` is short for a CXL Host-Managed Device Memory (HDM) Decoder. It is
a device that routes accesses through the CXL fabric to an endpoint, and at
the endpoint translates a `Host Physical` to `Device Physical` Addressing.

The CXL 3.1 specification heavily implies that only endpoint decoders should
engage in translation of `Host Physical Address` to `Device Physical Address`.
::

  8.2.4.20 CXL HDM Decoder Capability Structure

  IMPLEMENTATION NOTE
  CXL Host Bridge and Upstream Switch Port Decode Flow

  IMPLEMENTATION NOTE
  Device Decode Logic

These notes imply that there are two logical groups of decoders.

* Routing Decoder - a decoder which routes accesses but does not translate
  addresses from HPA to DPA.

* Translating Decoder - a decoder which translates accesses from HPA to DPA
  for an endpoint to service.

The CXL drivers distinguish 3 decoder types: root, switch, and endpoint. Only
endpoint decoders are Translating Decoders, all others are Routing Decoders.

.. note:: PLATFORM VENDORS BE AWARE

   Linux makes a strong assumption that endpoint decoders are the only decoder
   in the fabric that actively translates HPA to DPA.  Linux assumes routing
   decoders pass the HPA unchanged to the next decoder in the fabric.

   It is therefore assumed that any given decoder in the fabric will have an
   address range that is a subset of its upstream port decoder. Any deviation
   from this scheme undefined per the specification.  Linux prioritizes
   spec-defined / architectural behavior.

Decoders may have one or more `Downstream Targets` if configured to interleave
memory accesses.  This will be presented in sysfs via the :code:`target_list`
parameter.

Root Decoder
~~~~~~~~~~~~
A `Root Decoder` is logical construct of the physical address and interleave
configurations present in the CFMWS field of the :doc:`CEDT
<../platform/acpi/cedt>`.
Linux presents this information as a decoder present in the `CXL Root`.  We
consider this a `Root Decoder`, though technically it exists on the boundary
of the CXL specification and platform-specific CXL root implementations.

Linux considers these logical decoders a type of `Routing Decoder`, and is the
first decoder in the CXL fabric to receive a memory access from the platform's
memory controllers.

`Root Decoders` are created during :code:`cxl_acpi_probe`.  One root decoder
is created per CFMWS entry in the :doc:`CEDT <../platform/acpi/cedt>`.

The :code:`target_list` parameter is filled by the CFMWS target fields. Targets
of a root decoder are `Host Bridges`, which means interleave done at the root
decoder level is an `Inter-Host-Bridge Interleave`.

Only root decoders are capable of `Inter-Host-Bridge Interleave`.

Such interleaves must be configured by the platform and described in the ACPI
CEDT CFMWS, as the target CXL host bridge UIDs in the CFMWS must match the CXL
host bridge UIDs in the CHBS field of the :doc:`CEDT
<../platform/acpi/cedt>` and the UID field of CXL Host Bridges defined in
the :doc:`DSDT <../platform/acpi/dsdt>`.

Interleave settings in a root decoder describe how to interleave accesses among
the *immediate downstream targets*, not the entire interleave set.

The memory range described in the root decoder is used to

1) Create a memory region (:code:`region0` in this example), and

2) Associate the region with an IO Memory Resource (:code:`kernel/resource.c`)

::

  # ls /sys/bus/cxl/devices/decoder0.0/
    cap_pmem           devtype                 region0
    cap_ram            interleave_granularity  size
    cap_type2          interleave_ways         start
    cap_type3          locked                  subsystem
    create_ram_region  modalias                target_list
    delete_region      qos_class               uevent

  # cat /sys/bus/cxl/devices/decoder0.0/region0/resource
    0xc050000000

The IO Memory Resource is created during early boot when the CFMWS region is
identified in the EFI Memory Map or E820 table (on x86).

Root decoders are defined as a separate devtype, but are also a type
of `Switch Decoder` due to having downstream targets. ::

  # cat /sys/bus/cxl/devices/decoder0.0/devtype
    cxl_decoder_root

Switch Decoder
~~~~~~~~~~~~~~
Any non-root, translating decoder is considered a `Switch Decoder`, and will
present with the type :code:`cxl_decoder_switch`. Both `Host Bridge` and `CXL
Switch` (device) decoders are of type :code:`cxl_decoder_switch`. ::

  # ls /sys/bus/cxl/devices/decoder1.0/
    devtype                 locked    size       target_list
    interleave_granularity  modalias  start      target_type
    interleave_ways         region    subsystem  uevent

  # cat /sys/bus/cxl/devices/decoder1.0/devtype
    cxl_decoder_switch

  # cat /sys/bus/cxl/devices/decoder1.0/region
    region0

A `Switch Decoder` has associations between a region defined by a root
decoder and downstream target ports.  Interleaving done within a switch decoder
is a multi-downstream-port interleave (or `Intra-Host-Bridge Interleave` for
host bridges).

Interleave settings in a switch decoder describe how to interleave accesses
among the *immediate downstream targets*, not the entire interleave set.

Switch decoders are created during :code:`cxl_switch_port_probe` in the
:code:`cxl_port` driver, and is created based on a PCI device's DVSEC
registers.

Switch decoder programming is validated during probe if the platform programs
them during boot (See `Auto Decoders` below), or on commit if programmed at
runtime (See `Runtime Programming` below).


Endpoint Decoder
~~~~~~~~~~~~~~~~
Any decoder attached to a *terminal* point in the CXL fabric (`An Endpoint`) is
considered an `Endpoint Decoder`. Endpoint decoders are of type
:code:`cxl_decoder_endpoint`. ::

  # ls /sys/bus/cxl/devices/decoder5.0
    devtype                 locked    start
    dpa_resource            modalias  subsystem
    dpa_size                mode      target_type
    interleave_granularity  region    uevent
    interleave_ways         size

  # cat /sys/bus/cxl/devices/decoder5.0/devtype
    cxl_decoder_endpoint

  # cat /sys/bus/cxl/devices/decoder5.0/region
    region0

An `Endpoint Decoder` has an association with a region defined by a root
decoder and describes the device-local resource associated with this region.

Unlike root and switch decoders, endpoint decoders translate `Host Physical` to
`Device Physical` address ranges.  The interleave settings on an endpoint
therefore describe the entire *interleave set*.

`Device Physical Address` regions must be committed in-order. For example, the
DPA region starting at 0x80000000 cannot be committed before the DPA region
starting at 0x0.

As of Linux v6.15, Linux does not support *imbalanced* interleave setups, all
endpoints in an interleave set are expected to have the same interleave
settings (granularity and ways must be the same).

Endpoint decoders are created during :code:`cxl_endpoint_port_probe` in the
:code:`cxl_port` driver, and is created based on a PCI device's DVSEC registers.

Decoder Relationships
~~~~~~~~~~~~~~~~~~~~~
In our example described above, there is one root decoder which routes memory
accesses over two host bridges.  Each host bridge has a decoder which routes
access to their singular endpoint targets.  Each endpoint has a decoder which
translates HPA to DPA and services the memory request.

The driver validates relationships between ports by decoder programming, so
we can think of decoders being related in a similarly hierarchical fashion to
ports.

.. kernel-render:: DOT
   :alt: Digraph of hierarchical relationship between root, switch, and endpoint decoders.
   :caption: Diagraph of CXL root, switch, and endpoint decoders.

   digraph foo {
     "root0"    -> "decoder0.0";
     "decoder0.0" -> "decoder1.0";
     "decoder0.0" -> "decoder3.0";
     "decoder1.0" -> "decoder5.0";
     "decoder3.0" -> "decoder6.0";
   }

Regions
-------

Memory Region
~~~~~~~~~~~~~
A `Memory Region` is a logical construct that connects a set of CXL ports in
the fabric to an IO Memory Resource.  It is ultimately used to expose the memory
on these devices to the DAX subsystem via a `DAX Region`.

An example RAM region: ::

  # ls /sys/bus/cxl/devices/region0/
    access0      devtype                 modalias  subsystem  uuid
    access1      driver                  mode      target0
    commit       interleave_granularity  resource  target1
    dax_region0  interleave_ways         size      uevent

A memory region can be constructed during endpoint probe, if decoders were
programmed by BIOS/EFI (see `Auto Decoders`), or by creating a region manually
via a `Root Decoder`'s :code:`create_ram_region` or :code:`create_pmem_region`
interfaces.

The interleave settings in a `Memory Region` describe the configuration of the
`Interleave Set` - and are what can be expected to be seen in the endpoint
interleave settings.

.. kernel-render:: DOT
   :alt: Digraph of CXL memory region relationships between root and endpoint decoders.
   :caption: Regions are created based on root decoder configurations. Endpoint decoders
             must be programmed with the same interleave settings as the region.

   digraph foo {
     "root0"    -> "decoder0.0";
     "decoder0.0" -> "region0";
     "region0" -> "decoder5.0";
     "region0" -> "decoder6.0";
   }

DAX Region
~~~~~~~~~~
A `DAX Region` is used to convert a CXL `Memory Region` to a DAX device. A
DAX device may then be accessed directly via a file descriptor interface, or
converted to System RAM via the DAX kmem driver.  See the DAX driver section
for more details. ::

  # ls /sys/bus/cxl/devices/dax_region0/
    dax0.0      devtype  modalias   uevent
    dax_region  driver   subsystem

Mailbox Interfaces
------------------
A mailbox command interface for each device is exposed in ::

  /dev/cxl/mem0
  /dev/cxl/mem1

These mailboxes may receive any specification-defined command. Raw commands
(custom commands) can only be sent to these interfaces if the build config
:code:`CXL_MEM_RAW_COMMANDS` is set.  This is considered a debug and/or
development interface, not an officially supported mechanism for creation
of vendor-specific commands (see the `fwctl` subsystem for that).

Decoder Programming
===================

Runtime Programming
-------------------
During probe, the only decoders *required* to be programmed are `Root Decoders`.
In reality, `Root Decoders` are a logical construct to describe the memory
region and interleave configuration at the host bridge level - as described
in the ACPI CEDT CFMWS.

All other `Switch` and `Endpoint` decoders may be programmed by the user
at runtime - if the platform supports such configurations.

This interaction is what creates a `Software Defined Memory` environment.

See the :code:`cxl-cli` documentation for more information about how to
configure CXL decoders at runtime.

Auto Decoders
-------------
Auto Decoders are decoders programmed by BIOS/EFI at boot time, and are
almost always locked (cannot be changed).  This is done by a platform
which may have a static configuration - or certain quirks which may prevent
dynamic runtime changes to the decoders (such as requiring additional
controller programming within the CPU complex outside the scope of CXL).

Auto Decoders are probed automatically as long as the devices and memory
regions they are associated with probe without issue.  When probing Auto
Decoders, the driver's primary responsibility is to ensure the fabric is
sane - as-if validating runtime programmed regions and decoders.

If Linux cannot validate auto-decoder configuration, the memory will not
be surfaced as a DAX device - and therefore not be exposed to the page
allocator - effectively stranding it.

Interleave
----------

The Linux CXL driver supports `Cross-Link First` interleave. This dictates
how interleave is programmed at each decoder step, as the driver validates
the relationships between a decoder and it's parent.

For example, in a `Cross-Link First` interleave setup with 16 endpoints
attached to 4 host bridges, linux expects the following ways/granularity
across the root, host bridge, and endpoints respectively.

.. flat-table:: 4x4 cross-link first interleave settings

  * - decoder
    - ways
    - granularity

  * - root
    - 4
    - 256

  * - host bridge
    - 4
    - 1024

  * - endpoint
    - 16
    - 256

At the root, every a given access will be routed to the
:code:`((HPA / 256) % 4)th` target host bridge. Within a host bridge, every
:code:`((HPA / 1024) % 4)th` target endpoint.  Each endpoint translates based
on the entire 16 device interleave set.

Unbalanced interleave sets are not supported - decoders at a similar point
in the hierarchy (e.g. all host bridge decoders) must have the same ways and
granularity configuration.

At Root
~~~~~~~
Root decoder interleave is defined by CFMWS field of the :doc:`CEDT
<../platform/acpi/cedt>`.  The CEDT may actually define multiple CFMWS
configurations to describe the same physical capacity, with the intent to allow
users to decide at runtime whether to online memory as interleaved or
non-interleaved. ::

             Subtable Type : 01 [CXL Fixed Memory Window Structure]
       Window base address : 0000000100000000
               Window size : 0000000100000000
  Interleave Members (2^n) : 00
     Interleave Arithmetic : 00
              First Target : 00000007

             Subtable Type : 01 [CXL Fixed Memory Window Structure]
       Window base address : 0000000200000000
               Window size : 0000000100000000
  Interleave Members (2^n) : 00
     Interleave Arithmetic : 00
              First Target : 00000006

             Subtable Type : 01 [CXL Fixed Memory Window Structure]
       Window base address : 0000000300000000
               Window size : 0000000200000000
  Interleave Members (2^n) : 01
     Interleave Arithmetic : 00
              First Target : 00000007
               Next Target : 00000006

In this example, the CFMWS defines two discrete non-interleaved 4GB regions
for each host bridge, and one interleaved 8GB region that targets both. This
would result in 3 root decoders presenting in the root. ::

  # ls /sys/bus/cxl/devices/root0/decoder*
    decoder0.0  decoder0.1  decoder0.2

  # cat /sys/bus/cxl/devices/decoder0.0/target_list start size
    7
    0x100000000
    0x100000000

  # cat /sys/bus/cxl/devices/decoder0.1/target_list start size
    6
    0x200000000
    0x100000000

  # cat /sys/bus/cxl/devices/decoder0.2/target_list start size
    7,6
    0x300000000
    0x200000000

These decoders are not runtime programmable.  They are used to generate a
`Memory Region` to bring this memory online with runtime programmed settings
at the `Switch` and `Endpoint` decoders.

At Host Bridge or Switch
~~~~~~~~~~~~~~~~~~~~~~~~
`Host Bridge` and `Switch` decoders are programmable via the following fields:

- :code:`start` - the HPA region associated with the memory region
- :code:`size` - the size of the region
- :code:`target_list` - the list of downstream ports
- :code:`interleave_ways` - the number downstream ports to interleave across
- :code:`interleave_granularity` - the granularity to interleave at.

Linux expects the :code:`interleave_granularity` of switch decoders to be
derived from their upstream port connections. In `Cross-Link First` interleave
configurations, the :code:`interleave_granularity` of a decoder is equal to
:code:`parent_interleave_granularity * parent_interleave_ways`.

At Endpoint
~~~~~~~~~~~
`Endpoint Decoders` are programmed similar to Host Bridge and Switch decoders,
with the exception that the ways and granularity are defined by the interleave
set (e.g. the interleave settings defined by the associated `Memory Region`).

- :code:`start` - the HPA region associated with the memory region
- :code:`size` - the size of the region
- :code:`interleave_ways` - the number endpoints in the interleave set
- :code:`interleave_granularity` - the granularity to interleave at.

These settings are used by endpoint decoders to *Translate* memory requests
from HPA to DPA.  This is why they must be aware of the entire interleave set.

Linux does not support unbalanced interleave configurations.  As a result, all
endpoints in an interleave set must have the same ways and granularity.

Dynamic Capacity Extents
========================

A `Dynamic Capacity Device (DCD)` advertises capacity in `DC Regions`
and surfaces individual chunks of that capacity to the host as
`extents` (CXL r4.0 Section 9.13.3 "Dynamic Capacity Device (DCD)").
Linux calls a DC Region a `DC partition` (see "DPA Partitions" above)
so that "region" keeps its software meaning: a :code:`cxl_region` is
the host-side object that maps DPA into a host physical address range,
whereas a DC Region is a device-defined slice of DPA space.

Accepting Extents
-----------------
Extents are made available to the host through DC Add events: the
device sends one or more Dynamic Capacity Event Records (CXL r4.0
Section 8.2.10.2.1.6, Table 8-229) to the host.  Each record carries
one extent, which the host may accept or reject.  A device offering
several extents sends one record per extent, chained with `More`: each
record has the `More` flag set to announce another incoming record,
until the last record, which carries `More=0` to mark the end of the
chain.

For each record received, the host stages the offered extent on a
pending list.  When the chain closes the host knows that every extent
of this add event has arrived, and the whole pending list is validated
before anything is accepted.  A chain whose closing record does not
arrive within 20 seconds is refused as a whole: the watchdog answers
with an empty Add Dynamic Capacity Response so the device can reclaim
the offer, and the staged extents are discarded.

From the pending list, extents that share a (non-null) tag are
extracted into a tag group so that each group can be validated as a
unit; a group that is accepted becomes a :code:`struct
cxl_dc_tag_group`.  A tag group is one logical allocation.  An untagged
extent is an allocation of its own and becomes a single-member group.

Group-level validation (:code:`cxl_validate_group()`,
:code:`drivers/cxl/core/mbox.c`) drops the whole group if any check
fails:

* `Host-wide tag uniqueness`: a non-null tag must not already
  correspond to a live :code:`cxl_dc_tag_group` anywhere on this host.
  The orchestrator / Fabric Manager owns tag assignment (CXL r4.0
  Section 9.13.3.1 "DCD Management by FM" and Section 7.6.7.6 "DCD
  Management Command Set for LD-FAM"); the list in
  :code:`drivers/cxl/core/extent.c` (:code:`cxl_tag_register()` /
  :code:`cxl_tag_already_committed()`) catches firmware bugs and
  orchestrator misbehavior across every region and memdev.  Skipped
  for the null UUID because there may be multiple untagged extents.
* `Sequence-number integrity`: extents may be shared across hosts if
  the device supports memory sharing.  Since a tag group is one
  logical allocation, every host needs to see the extents of a shared
  group in the same order; that order is the Shared Extent Sequence
  Number of each extent (:code:`shared_extn_seq`, Table 8-230).  A
  sharable group's sequence numbers must form exactly
  :code:`0, 1, ..., n-1`: sequences with gaps or duplicates, or that
  do not start at 0, are rejected.  A non-sharable extent must carry
  0 in the field; that is rejected per extent by
  :code:`cxl_validate_extent()`, so the group check covers only
  sharable partitions and the host orders non-sharable extents by
  arrival.
* `Partition equality`: every tagged extent in the group must resolve
  to the same DC partition.  A single allocation cannot span
  partitions because CDAT describes sharable / writable / coherency
  attributes per partition.  Skipped for the null UUID.
* `Alignment`: every extent's :code:`start_dpa` and :code:`length`
  must be :code:`PMD_SIZE`-aligned, the alignment the DAX region is
  created with.  If any extent is misaligned the whole group is
  dropped: accepting only the aligned subset would leave an unusable
  allocation.

After group-level validation each extent is validated individually
(:code:`cxl_add_extent()`, :code:`drivers/cxl/core/extent.c`).  A tag
group is accepted whole or not at all: for sharable capacity the spec
requires it (Section 8.2.10.9.9.3, otherwise the device returns Invalid
Extent List), and an allocation missing an extent would be unusable
either way.  So an extent that fails any of these checks drops its
whole group:

* The extent's DPA range must lie within a DC partition of the memdev
  (:code:`cxl_extent_dc_partition()`).  On a sharable partition the
  extent must carry a tag; on a non-sharable one its shared extent
  sequence must be 0.
* The extent's DPA range must resolve to a CXL region via
  :code:`cxl_dpa_to_region()`.  An extent with no owning region is
  dropped, as is one whose :code:`dax_region` has not finished its
  probe-time scan or has no driver bound (see Recovery below).
* The extent's DPA range must be fully contained in the endpoint
  decoder's DPA range.  An extent that straddles the decoder boundary
  is dropped; the driver never clips an extent to fit.
* The extent must not overlap or repeat an extent already present in
  the same region (:code:`cxlr_dax_classify_extent()`).  A range the
  host already accepted drops its group: the device rejects a DPA
  range reported twice in an Add response (CXL r4.0 8.2.10.9.9.3,
  Invalid Physical Address).

Surviving tag groups are registered with the device model by
:code:`online_tag_group()`: a tagged group becomes a
:code:`tag_groupX.Y` child of the :code:`dax_region` device and every
member :code:`dc_extent` an :code:`extentX.Y` child of it, while an
untagged extent goes under the region's :code:`untagged` device.  The
host then tells the device which extents it accepted with an Add
Dynamic Capacity Response (CXL r4.0 Section 8.2.10.9.9.3, opcode 4802h)
per chain, split across several commands chained with the More flag
when the extent list outgrows the mailbox payload; an extent left out
of the response is rejected.

Extents the device already reserves for a region, for instance after a
host crash, are recovered when the region's :code:`dax_region` probes:
:code:`cxl_region_add_existing_extents()` reads the device's extent
list (Get Dynamic Capacity Extent List, Section 8.2.10.9.9.2, opcode
4801h), keeps the extents inside the region's decoder (the list covers
every DC partition of the device) and runs them through the same
validation with :code:`existing` set.  No Add Dynamic Capacity Response
is sent for them, and a recovered group that fails validation or does
not online is released back to the device.  Add events for the region
are refused until that scan completes, and again once the driver
unbinds: unbinding returns every held extent to the device, so the next
probe scans afresh.

Releasing Extents
-----------------

A release may be initiated by the device (a Release Capacity event) or
by the host, for example when tearing down a region.  A host may also
issue a Release Dynamic Capacity Response not associated with any
event, an "unsolicited release", at any time (CXL r4.0 Section 9.13.3).
The host-initiated path runs :code:`dc_tag_group_teardown()` for each
tag group of the region: it invalidates the region's cached lines,
returns the group with its own Release Dynamic Capacity Response and
unregisters its extents.  The device-initiated path goes through the
release chain in :code:`drivers/cxl/core/mbox.c`.

A device-initiated release may span several event records chained with
the More flag, exactly like an add event.  Each record is staged in
:code:`mds->dc_chain` by :code:`handle_dcd_chain_event()`, the same
chain an add event uses, until the chain closes (More flag cleared).
Chained records shall be contiguous and share one event type (CXL r4.0
Table 8-229), so add and release records cannot coincide; should a
record of the other type arrive inside an open chain, the open chain is
aborted (an add chain with an empty Add Dynamic Capacity Response, a
release chain silently) and the record starts a new chain.  A chain
whose closing record never arrives is dropped by the 20 second watchdog
the same way, so the capacity stays provisioned for the host and the
device may retry.

When the chain closes, :code:`cxl_rm_pending()` processes the extents
staged in the pending list.  As mentioned above, a tag group is one
logical allocation, so requesting the release of any extent in a group
releases the whole group.  Members of the group missing from the event
record chain (an FM mistake or firmware bug) are still included in the
Release Dynamic Capacity Response as a "generous" policy.

Each extent pending release is processed:

* The extent is resolved by :code:`cxl_resolve_extent()` to its CXL
  region and :code:`cxl_dc_tag_group`.
* An extent the host does not own is acknowledged so the device knows
  the host is not using it: its DPA range resolves to no region, to a
  region whose :code:`cxlr_dax` is not set up, or to a range that
  overlaps nothing the host owns (for example, an extent left over
  from a host crash that has not yet been re-claimed, or a duplicate
  release racing region teardown).
* An extent that overlaps capacity the host owns without matching any
  single accepted extent (it straddles two extents, or the tag
  disagrees) is held: it is left out of the response so the device
  retains the capacity for the host and may retry.
* An extent that names an owned extent releases its group through
  :code:`cxl_release_tag_group()`: the backing memregion is
  invalidated once for the group and :code:`rm_tag_group()`
  unregisters every member :code:`dc_extent` device.  If the
  invalidation fails, or the :code:`dax_region`'s unbind already owns
  the group's teardown, the group is left out of the response, so the
  device retains the capacity and may retry.

After processing the pending list, every extent the host is giving
back is sent to the device in the Release Dynamic Capacity Response
(Section 8.2.10.9.9.4, opcode 4803h), mirroring the Add response.  A
list that does not fit in one mailbox payload is sent as several
commands chained with the More flag.

Example Configurations
======================
.. toctree::
   :maxdepth: 1

   example-configurations/single-device.rst
   example-configurations/hb-interleave.rst
   example-configurations/intra-hb-interleave.rst
   example-configurations/multi-interleave.rst
