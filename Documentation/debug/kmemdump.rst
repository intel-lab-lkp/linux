.. SPDX-License-Identifier: GPL-2.0

==========================
kmemdump
==========================

This document provides information about the kmemdump feature.

Overview
========

kmemdump is a mechanism that allows any driver or producer to register a
chunk of memory into kmemdump, to be used at a later time for a specific
purpose like debugging or memory dumping.

kmemdump allows a backend to be connected, this backend interfaces a
specific hardware that can debug or dump the memory registered into
kmemdump.

kmemdump can also prepare specific regions of the kernel that can be
put together to form a minimal core image file. To achieve this, the first
region is an ELF header with program headers for each region, and specific
ELF NOTE section with vmcoreinfo. To enable this feature, use
`CONFIG_DRIVER_KMEMDUMP_COREIMAGE`.

kmemdump Internals
=============

API
----

A memory region is being registered with a call to `kmemdump_register` which
takes as parameters the name of the region, a pointer to the virtual memory
start address and the size. If successful, this call returns an unique ID for
the allocated zone.

The region would be registered with a call to `kmemdump_unregister` which
takes the id as a parameter.

Backend
-------

Backend is represented by a `struct kmemdump_backend` which has to be filled
in by the backend driver. Further, this struct is being passed to kmemdump
with a `backend_register` call. `backend_unregister` will remove the backend
from kmemdump.

Once a backend is being registered, all previously registered regions are
being sent to the backend for registration.

When the backend is being removed, all regions are being first deregistered
from the backend.

kmemdump will request the backend to register a region with `register_region`
call, and deregister a region with `unregister_region` call. These two
functions are mandatory to be provided by a backend at registration time.

Data structures
---------------

`struct kmemdump_backend` represents the kmemdump backend and it has two
function pointers, one called `register_region` and the other
`unregister_region`.

Each region is being stored into a cyclic array of unique ids called
`kmemdump_idr`.

kmemdump Initialization
------------------

After system boots, kmemdump will be ready to accept region registration
from producer drivers. However, the backend may not be registered yet.
These regions are being added to the internal list and pending backend
initialization. Once the backend is up and running, all the regions are
registered into the backend. If, for example, the backend becomes unavailable
and is removed, all the regions are unregistered and kmemdump waits for
another backend to become available.

backend functionality
-----------------

kmemdump backend can keep it's own list of regions and use the specific
hardware available to dump the memory regions or use them for debugging.
