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


Resolve conflict between CFMWS, Plaftform Memory Holes, and Endpoint Decoders
=============================================================================

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

According to the current CXL Specifications (Revision 3.2, Version 1.0)
the CXL Fixed Memory Window Structure (CFMWS) describes zero or more Host
Physical Address (HPA) windows that are associated with each CXL Host
Bridge. Each window represents a contiguous HPA range that may be
interleaved across one or more targets, some of which are CXL Host Bridges.
Associated with each window are a set of restrictions that govern its
usage. It is the OSPM’s responsibility to utilize each window for the
specified use.

Table 9-22 states the Window Size field contains that the total number of
consecutive bytes of HPA this window represents and this value shall be a
multiple of Number of Interleave Ways * 256 MB.

Platform Firmware (BIOS) might reserve part of physical addresses below
4 GB (e.g., the Low Memory Hole that describes PCIe memory space for MMIO
or a requirement for the greater than 8 way interleave CXL regions starting
at address 0). In that case the Window Size value cannot be anymore
constrained to the NIW * 256 MB above-mentioned rule.

On those systems, BIOS publishes CFMWS which communicate the active System
Physical Address (SPA) ranges that map to a subset of the Host Physical
Address (HPA) ranges. The SPA range trims out the hole, and capacity in the
endpoint is lost with no SPA to map to CXL HPA in that hole.

The description of the Window Size field in table 9-22 needs to take that
special case into account.

Note that the Endpoint Decoders HPA range sizes have to comply with the
alignment constraints and so a part of their memory capacity might not be
accessible if their size exceeds the matching CFMWS range's.

Benefits of the Change
----------------------

Without this change, the OSPM wouldn't match Endpoint Decoders with CFMWS
whose Window Size don't comply with the alignment rules and so all their
capacity would be lost. This change allows the OSPM to match Endpoint
Decoders whose HPA range size exceeds the matching CFMWS and create
regions that at least utilize part of the decoders total memory capacity.

References
----------

Compute Express Link Specification Revision 3.2, Version 1.0
<https://www.computeexpresslink.org/>

Detailed Description of the Change
----------------------------------

The current description of a CFMWS Window Size (table 9-22) is replaced
with:

"The total number of consecutive bytes of HPA this window represents. This
value shall be a multiple of NIW*256 MB. On platforms that reserve physical
addresses below 4 GB for special use (e.g., the Low Memory Hole for PCIe
MMIO on x86), an instance of CFMWS whose Base HPA is 0 might have a window
size that doesn't align with the NIW*256 MB constraint; note that the
matching Endpoint Decoders HPA range size must still align to the
above-mentioned rule and so the memory capacity that might exceeds the
CFMWS window size will not be accessible.".
