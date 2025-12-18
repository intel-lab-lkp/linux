.. SPDX-License-Identifier: GPL-2.0

==================
CXL Device Hotplug
==================

Device hotplug refers to *physical* hotplug of a device (addition or removal
of a physical device from the machine).

Hot-Remove
==========
Hot removal of a device typically requires careful removal of software
constructs (memory regions, associated drivers) which manage these devices.

Hard-removing a CXL.mem device without carefully tearing down driver stacks
is likely to cause the system to machine-check (or at least SIGBUS if memory
access is limited to user space).

Memory Device Hot-Add
=====================
Hot-adding a memory device requires that the memory associated with that
device fits in a pre-defined (*static*) CXL Fixed Memory Window in the
:doc:`CEDT<acpi/cedt>`.

There are two basic hot-add scenarios which may occur.

Device Present at Boot
----------------------
A device present at boot likely had its capacity reported in the
:doc:`CEDT<acpi/cedt>`.  If a device is removed and a new device hotplugged,
the capacity of the new device will be limited to the original CFMWS capacity.

Adding a device larger than the original device will cause memory region
creation to fail if the region size is greater than the CFMWS size.

The CFMWS is *static* and cannot be adjusted.  Platforms which may expect
different sized devices to be hotplugged must allocate sufficient CFMWS space
*at boot time* to cover all future expected devices.

No CXL Device Present at Boot
-----------------------------
When no CXL device is present on boot, most platforms omit the CFMWS in the
:doc:`CEDT<acpi/cedt>`.  When this occurs, hot-add is not possible.

For a platform to support hot-add of a memory device, it must allocate a
CEDT CFMWS region with sufficient memory capacity to cover all future
potentially added capacity.

Switches in the fabric should report the max possible memory capacity
expected to be hot-added so that platform software may construct the
appropriately sized CFMWS.

Interleave Sets
===============

Host Bridge Interleave
----------------------
Host-bridge interleaved memory regions are defined *statically* in the
:doc:`CEDT<acpi/cedt>`.  To apply cross-host-bridge interleave, a CFMWS entry
describing that interleave must have been provided *at boot*.  Hotplugged
devices cannot add host-bridge interleave capabilities at hotplug time.

See the :doc:`Flexible CEDT Configuration<example-configurations/flexible>`
example to see how a platform can provide this kind of flexibility regarding
hotplugged memory devices.

Platform vendors should work with switch vendors to work out how this
HPA space reservation should work when one or more interleave options are
intended to be presented to a host.

HDM Interleave
--------------
Decoder-applied interleave can flexibly handle hotplugged devices, as decoders
can be re-programmed after hotplug.

To add or remove a device to/from an existing HDM-applied interleaved region,
that region must be torn down an re-created.
