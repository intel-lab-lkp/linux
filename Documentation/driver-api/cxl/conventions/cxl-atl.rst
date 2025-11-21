.. SPDX-License-Identifier: GPL-2.0
.. include:: <isonum.txt>

ACPI PRM CXL Address Translation
================================

Document
--------

CXL Revision 3.2, Version 1.0

License
-------

SPDX-License Identifier: CC-BY-4.0

Creator/Contributors
--------------------

- Robert Richter, AMD

Summary of the Change
---------------------

The CXL Fixed Memory Window Structure (CFMWS) describes zero or more
Host Physical Address (HPA) windows that are associated with each CXL
Host Bridge. The HPA ranges of an CFMWS may include addresses that are
currently assigned to CXL.mem devices, or an OS may assign ranges from
an address window to a device.

Host-managed Device Memory is Device-attached memory that is mapped to
system coherent address space and accessible to the Host using
standard write-back semantics. The managed address range is configured
in the CXL HDM Decoder registers of the device. An HDM Decoder in a
device is responsible for converting HPA into DPA by stripping off
specific address bits.

CXL devices and CXL bridges use the same HPA space. It is common
across all components that belong to the same host domain. The view of
the address region must be consistent on the CXL.mem path between the
Host and the Device.

This is described in the current CXL specification (Table 1-1, 3.3.1,
8.2.4.20, 9.13.1, 9.18.1.3). [#cxl-spec-3.2]_

Depending on the interconnect architecture of the platform, components
attached to a host may not share the same host physical address space.
Those platforms need address translation to convert an HPA between the
host and the attached component, such as a CXL device. The translation
mechanism is host-specific and implementation dependent.

E.g., x86 AMD platforms use a Data Fabric that manages access to
physical memory. Devices have an own memory space and can be
configured to use 'Normalized addresses' different to System Physical
Addresses (SPA). Address translation is needed then. Details are
described also under x86 AMD
Documentation/admin-guide/RAS/address-translation.rst.

Those AMD platforms provide PRM handlers in firmware to perform
various types of address translation, including for CXL endpoints.
AMD Zen5 systems implement the ACPI PRM CXL Address Translation
firmware call. The ACPI PRM handler has a specific GUID to uniquely
identify platforms with support of Normalized addressing. This is
documented in the ACPI v6.5 Porting Guide, Address Translation - CXL
DPA to System Physical Address.  [#amd-ppr-58088]_

When in Normalized address mode, HDM decoder address ranges must be
configured and handled differently. Hardware addresses used in the HDM
decoder configurations of an endpoint are not SPA and need to be
translated from the endpoint's to its CXL host bridge's address range.
This is esp. important to find an endpoint's associated CXL Host
Bridge and HPA window described in the CFMWS. Also, the interleave
decoding is done by the Data Fabric and the endpoint does not perform
decoding when converting HPA to DPA. Instead, interleaving is switched
off for the endpoint (1 way). Finally, address translation might also
be needed to inspect the Endpoint's hardware addresses, such as during
profiling, tracing or error handling.

For example, with Normalized addressing the HDM decoders could look as
following:

.. code-block:: none

 /sys/bus/cxl/devices/endpoint5/decoder5.0/interleave_granularity:256
 /sys/bus/cxl/devices/endpoint5/decoder5.0/interleave_ways:1
 /sys/bus/cxl/devices/endpoint5/decoder5.0/size:0x2000000000
 /sys/bus/cxl/devices/endpoint5/decoder5.0/start:0x0
 /sys/bus/cxl/devices/endpoint8/decoder8.0/interleave_granularity:256
 /sys/bus/cxl/devices/endpoint8/decoder8.0/interleave_ways:1
 /sys/bus/cxl/devices/endpoint8/decoder8.0/size:0x2000000000
 /sys/bus/cxl/devices/endpoint8/decoder8.0/start:0x0
 /sys/bus/cxl/devices/endpoint11/decoder11.0/interleave_granularity:256
 /sys/bus/cxl/devices/endpoint11/decoder11.0/interleave_ways:1
 /sys/bus/cxl/devices/endpoint11/decoder11.0/size:0x2000000000
 /sys/bus/cxl/devices/endpoint11/decoder11.0/start:0x0
 /sys/bus/cxl/devices/endpoint13/decoder13.0/interleave_granularity:256
 /sys/bus/cxl/devices/endpoint13/decoder13.0/interleave_ways:1
 /sys/bus/cxl/devices/endpoint13/decoder13.0/size:0x2000000000
 /sys/bus/cxl/devices/endpoint13/decoder13.0/start:0x0

Note the endpoint interleaving configurations with a direct mapping
(1-way).

With PRM calls, the kernel can determine the following mappings:

.. code-block:: none

 cxl decoder5.0: address mapping found for 0000:e2:00.0 (hpa -> spa):
   0x0+0x2000000000 -> 0x850000000+0x8000000000 ways:4 granularity:256
 cxl decoder8.0: address mapping found for 0000:e3:00.0 (hpa -> spa):
   0x0+0x2000000000 -> 0x850000000+0x8000000000 ways:4 granularity:256
 cxl decoder11.0: address mapping found for 0000:e4:00.0 (hpa -> spa):
   0x0+0x2000000000 -> 0x850000000+0x8000000000 ways:4 granularity:256
 cxl decoder13.0: address mapping found for 0000:e1:00.0 (hpa -> spa):
   0x0+0x2000000000 -> 0x850000000+0x8000000000 ways:4 granularity:256

The corresponding CXL host bridge (HDM) decoders and root decoder
(CFMWS) show and match with the calculated endpoint mappings:

.. code-block:: none

 /sys/bus/cxl/devices/port1/decoder1.0/interleave_granularity:256
 /sys/bus/cxl/devices/port1/decoder1.0/interleave_ways:4
 /sys/bus/cxl/devices/port1/decoder1.0/size:0x8000000000
 /sys/bus/cxl/devices/port1/decoder1.0/start:0x850000000
 /sys/bus/cxl/devices/port1/decoder1.0/target_list:0,1,2,3
 /sys/bus/cxl/devices/port1/decoder1.0/target_type:expander
 /sys/bus/cxl/devices/root0/decoder0.0/interleave_granularity:256
 /sys/bus/cxl/devices/root0/decoder0.0/interleave_ways:1
 /sys/bus/cxl/devices/root0/decoder0.0/size:0x8000000000
 /sys/bus/cxl/devices/root0/decoder0.0/start:0x850000000
 /sys/bus/cxl/devices/root0/decoder0.0/target_list:7

The following changes of the specification are needed:

* Allow a CXL device to be in a different HPA space other than the
  host's space.

* The platform can use implementation-specific address translation
  when crossing memory domains on the CXL.mem path between the Host
  and the Device.

* The kernel (OSPM) determines Endpoint SPA range and interleaving
  configuration using platform specific address translation methods.

Benefits of the Change
----------------------

Without the change, the OSPM may not determine the memory region and
Root Decoder of an Endpoint and its corresponding HDM decoder. Region
creation would fail. Platforms with a different interconnect
architecture would fail to setup and use CXL.

References
----------

.. [#cxl-spec-3.2] Compute Express Link Specification, Revision 3.2, Version 1.0,
   https://www.computeexpresslink.org/

.. [#amd-ppr-58088] AMD Family 1Ah Models 00h–0Fh and Models 10h–1Fh,
   ACPI v6.5 Porting Guide, Publication # 58088,
   https://www.amd.com/en/search/documentation/hub.html

Detailed Description of the Change
----------------------------------

Add the following paragraph in 8.2.4.20 CXL HDM Decoder Capability
Structure of the specification [#cxl-spec-3.2]_ to the end:

"A device may use a different HPA space that is not common to other
components of the host domain. The platform is responsible for address
translation when crossing HPA spaces. The OSPM must determine the
interleaving configuration and perform address translation to HPA
ranges of the HDM decoders as needed. The translation mechanism is
host-specific and implementation dependent."
