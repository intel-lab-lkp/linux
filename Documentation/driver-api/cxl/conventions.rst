.. SPDX-License-Identifier: GPL-2.0
.. include:: <isonum.txt>

=======================================
Compute Express Link: Linux Conventions
=======================================

There exists shipping platforms that bend or break CXL specification
expectations. Record the details and the rationale for those deviations.
Borrow the ACPI Code First template format to capture the assumptions
and tradeoffs such that multiple platform implementations can follow the
same convention.

<(template) Title>
==================

Document
--------
CXL Revision <rev>, Version <ver>

License
-------
SPDX-License Identifier: CC-BY-4.0

Creator/Contributors
--------------------

Summary of the Change
---------------------

<Detail the conflict with the specification and where available the
assumptions and tradeoffs taken by the hardware platform.>


Benefits of the Change
----------------------

<Detail what happens if platforms and Linux do not adopt this
convention.>

References
----------

Detailed Description of the Change
----------------------------------

<Propose spec language that corrects the conflict.>


Resolve conflict between CFMWS, Platform Memory Holes, and Endpoint Decoders
============================================================================

Document
--------

CXL Revision 3.2, Version 1.0

License
-------

SPDX-License Identifier: CC-BY-4.0

Creator/Contributors
--------------------

Fabio M. De Francesco, Intel
Dan J. Williams, Intel
Mahesh Natu, Intel

Summary of the Change
---------------------

According to the current CXL Specifications (Revision 3.2, Version 1.0),
the CXL Fixed Memory Window Structure (CFMWS) describes zero or more Host
Physical Address (HPA) windows associated with each CXL Host Bridge. Each
window represents a contiguous HPA range that may be interleaved across
one or more targets, including CXL Host Bridges. Each window has a set of
restrictions that govern its usage. It is the OSPM’s responsibility to
utilize each window for the specified use.

Table 9-22 states the Window Size field contains the total number of
consecutive bytes of HPA this window represents. This value must be a
multiple of the Number of Interleave Ways * 256 MB.

Platform Firmware (BIOS) might reserve physical addresses below 4 GB,
such as the Low Memory Hole for PCIe MMIO. In such cases, the CFMWS Range
Size may not adhere to the NIW * 256 MB rule.

On these systems, BIOS publishes CFMWS to communicate the active System
Physical Address (SPA) ranges that map to a subset of the Host Physical
Address (HPA) ranges. The SPA range trims out the hole, resulting in lost
capacity in the endpoint with no SPA to map to the CXL HPA range that
exceeds the matching CFMWS range.

E.g, a real x86 platform with two CFMWS, 384 GB total memory, and LMH
starting at 2 GB:

Window | CFMWS Base | CFMWS Size | HDM Decoder Base | HDM Decoder Size | Ways | Granularity
  0    |   0 GB     |     2 GB   |      0 GB        |       3 GB       |  12  |    256
  1    |   4 GB     |   380 GB   |      0 GB        |     380 GB       |  12  |    256

HDM decoder base and HDM decoder size represent all the 12 Endpoint
Decoders of a 12 way region and all the intermediate Switch Decoders.
They are configured by the BIOS according to the NIW * 256MB rule,
resulting in a HPA range size of 3GB.

The CFMWS Base and CFMWS Size are used to configure the Root Decoder HPA
range base and size. CFMWS cannot intersect Memory Holes, then the CFMWS[0]
size is smaller (2GB) than that of the Switch and Endpoint Decoders that
make the hierarchy (3GB).

On that platform, only the first 2GB will be potentially usable but,
because of the current specs, Linux fails to make them available to the
users. The driver expects that Root Decoder HPA size, which is equal to
the CFMWS from which it is configured, to be greater or equal to the
matching Switch and Endpoint HDM Decoders.

The CXL driver fails to construct Regions and to attach Endpoint and
intermediate Switch Decoders to those Regions after their construction.

In order to succeed with Region construction and Decoders attachment,
Linux must construct Regions with Root Decoders size, and then attach to
them all the intermediate Switch and Endpoint Decoders that are part of the
hierarchy, even though the Decoders HPA range sizes may be larger than
those Regions whose sizes are trimmed by Low Memory Holes.

Benefits of the Change
----------------------

Without this change, the OSPM wouldn't match Intermediate and Endpoint
Decoders with Root Decoders configured with CFMWS HPA sizes that don't
align with the NIW * 256MB constraint, leading to lost memdev capacity.
This change allows the OSPM to construct Regions and attach Intermediate
Switch and Endpoint Decoders to them, so that the addressable part of the
memory devices total capacity is not lost.

References
----------

Compute Express Link Specification Revision 3.2, Version 1.0
<https://www.computeexpresslink.org/>

Detailed Description of the Change
----------------------------------

The description of the Window Size field in table 9-22 needs to account
for platforms with Low Memory Holes, where SPA ranges might be subsets of
the endpoints' HPA. Therefore, it has to be changed to the following:

"The total number of consecutive bytes of HPA this window represents.
This value shall be a multiple of NIW * 256 MB. On platforms that reserve
physical addresses below 4 GB, such as the Low Memory Hole for PCIe MMIO
on x86 or a requirement for greater than 8-way interleave CXL Regions
starting at address 0, an instance of CFMWS whose Base HPA is 0 might have
a window size that doesn't align with the NIW * 256 MB constraint. Note
that the matching intermediate Switch and Endpoint Decoders' HPA range
sizes must still align to the above-mentioned rule, but the memory capacity
that exceeds the CFMWS window size will not be accessible."
