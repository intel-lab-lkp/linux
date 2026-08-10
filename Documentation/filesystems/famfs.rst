.. SPDX-License-Identifier: GPL-2.0

.. _famfs_index:

==================================================================
famfs: The fabric-attached memory file system
==================================================================

- Copyright (C) 2024-2026 Micron Technology, Inc.

Introduction
============
Compute Express Link (CXL) provides a mechanism for disaggregated or
fabric-attached memory (FAM). This creates opportunities for data sharing;
clustered apps that would otherwise have to shard or replicate data can
share one copy in disaggregated memory.

Famfs, which is not CXL-specific in any way, provides a mechanism for
multiple hosts to concurrently access data in shared memory, by giving it
a file system interface. With famfs, any app that understands files can
access data sets in shared memory. Although famfs supports read and write,
the real point is to support mmap, which provides direct (dax) access to
the memory - either writable or read-only.

Shared memory can pose complex coherency and synchronization issues, but
there are also simple cases. Two simple and eminently useful patterns that
occur frequently in data analytics and AI are:

* Serial Sharing - Only one host or process at a time has access to a file
* Read-only Sharing - Multiple hosts or processes share read-only access
  to a file

The famfs file system is part of the famfs framework; user space
components [1] handle metadata allocation and distribution, and populate
the in-kernel files (via ioctls) so that they map directly to shared
memory.

The famfs framework manages coherency of its own metadata and structures,
but does not attempt to manage coherency for applications.

Famfs also provides data isolation between files. That is, even though
the host has access to an entire memory "device" (as a devdax device), apps
cannot write to memory for which the file is read-only, and mapping one
file provides isolation from the memory of all other files. This is pretty
basic, but some experimental shared memory usage patterns provide no such
isolation.

Principles of Operation
=======================

Famfs is a file system with one or more devdax devices as a first-class
backing device(s). Metadata maintenance and query operations happen
entirely in user space.

The famfs user space provides each file's map (fmap) and its backing
devdax devices to the kernel through ioctls (FAMFSIOC_MAP_CREATE and
FAMFSIOC_DAXDEV_OPEN), after which read/write/mapping faults are handled
entirely in the kernel with no up-calls for all active files.

The famfs user space is responsible for maintaining and distributing
consistent metadata. This is currently handled via an append-only
metadata log within the memory, but this is orthogonal to the famfs
kernel code.

Once instantiated, "the same file" on each host points to the same shared
memory, but in-memory metadata (inodes, etc.) is ephemeral on each host
that has a famfs instance mounted. Use cases are free to allow or not
allow mutations to data on a file-by-file basis.

When an app accesses a data object in a famfs file, there is no page cache
involvement. The CPU cache is loaded directly from the shared memory. In
some use cases, this is an enormous reduction in read amplification
compared to loading an entire page into the page cache.


Famfs is Not a Conventional File System
---------------------------------------

Famfs files can be accessed by conventional means, but there are
limitations. The famfs kernel module is not involved in the allocation of
backing memory for files at all; the famfs user space creates files and
supplies their fmaps and devdax device info to the kernel via ioctls.

Famfs differs in some important ways from conventional file systems:

* Files must be pre-allocated by the famfs framework; allocation is never
  performed on (or after) write.
* Any operation that changes a file's size is considered to put the file
  in an invalid state, disabling access to the data. It may be possible to
  revisit this in the future. (Typically the famfs user space can restore
  files to a valid state by replaying the famfs metadata log.)

Famfs exists to apply the existing file system abstractions to shared
memory so applications and workflows can more easily adapt to an
environment with disaggregated shared memory.

Memory Error Handling
=====================

Possible memory errors include timeouts, poison, and unexpected
reconfiguration of an underlying dax device. In all of these cases, famfs
receives a call from the devdax layer via its
dax_holder_operations->notify_failure() function. If any memory errors have
been detected, access to the affected
daxdev is disabled to avoid further errors or corruption.

In all known cases, famfs can be unmounted cleanly. In most cases errors
can be cleared by re-initializing the memory - at which point a new famfs
file system can be created.

Key Requirements
================

The primary requirements for famfs are:

1. Must support a file system abstraction backed by sharable devdax memory
2. Files must efficiently handle VMA faults
3. Must support metadata distribution in a sharable way
4. Must handle clients with a stale copy of metadata

The famfs kernel component takes care of 1-2 above by caching each file's
mapping metadata in the kernel.

Requirements 3 and 4 are handled by the user space components, and are
largely orthogonal to the functionality of the famfs kernel module.

Requirements 3 and 4 cannot be met by conventional fs-dax file systems
(e.g. xfs) because they use write-back metadata; it is not valid to mount
such a file system on two hosts from the same in-memory image.


Famfs Usage
===========

Famfs usage is documented at [1].


References
==========

- [1] Famfs user space repository and documentation
      https://github.com/cxl-micron-reskit/famfs
