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

kmemdump Internals
=============

API
----

A memory region is being registered with a call to `kmemdump_register` which
takes as parameters the ID of the region, a pointer to the virtual memory
start address and the size. If successful, this call returns an unique ID for
the allocated zone (either the requested ID or an allocated ID).
IDs are predefined in the kmemdump header. A second registration with the
same ID is not allowed, the caller needs to deregister first.
A dedicated NO_ID is defined, which has kmemdump allocate a new unique ID
for the request and return it. This case is useful with multiple dynamic
loop allocations where ID is not significant.

The region would be registered with a call to `kmemdump_unregister` which
takes the id as a parameter.

For dynamically allocated memory, kmemdump defines a variety of wrappers
on top of allocation functions which are given as parameters.
This makes the dynamic allocation easy to use without additional calls
to registration functions. However kmemdump still exposes the register API
for cases where it may be needed (e.g. size is not exactly known at allocation
time).

For static variables, a variety of annotation macros are provided. These
macros will create an annotation struct inside a separate section.


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
There is a default backend that does a no-op that is initially registered
and is registered back if the current working backend is being removed.

The regions are being stored in a simple fixed size array. It avoids
memory allocation overhead. This is not performance critical nor does
allocating a few hundred entries create a memory consumption problem.

The static variables registered into kmemdump are being annotated into
a dedicated `.kemdump` memory section. This is then walked by kmemdump
at a later time and each variable is registered.

kmemdump Initialization
------------------

After system boots, kmemdump will be ready to accept region registration
from producer drivers. Even if the backend may not be registered yet,
there is a default no-op backend that is registered. At any time the backend
can be changed with a real backend in which case all regions are being
registered to the new backend.

backend functionality
-----------------

kmemdump backend can keep it's own list of regions and use the specific
hardware available to dump the memory regions or use them for debugging.
