.. SPDX-License-Identifier: GPL-2.0

========================
Shared Memory Filesystem
========================

The shared memory filesystem (tmpfs, also known as shmem) provides an
in-memory filesystem used for ``/tmp`` mounts, POSIX shared memory
(``shm_open()``), System V shared memory, and anonymous shared mappings
created with ``mmap(MAP_SHARED | MAP_ANONYMOUS)``.  The implementation is
in ``mm/shmem.c``.

.. contents:: :local:

How It Works
============

tmpfs stores file contents in the page cache using swap as its backing
store rather than a disk filesystem.  Pages are allocated on demand when
written to or faulted in.  When the system is under memory pressure, tmpfs
pages can be swapped out just like anonymous pages.

This design means tmpfs files consume no disk space — their size is bounded
only by available memory and swap.  It also means tmpfs data does not
survive a reboot, making it suitable for scratch data that benefits from
memory-speed access without needing durability.

Each tmpfs inode tracks two key counters: allocated pages (resident in
memory) and swapped pages (evicted to swap).  These are maintained by
the ``shmem_charge()`` and ``shmem_uncharge()`` accounting functions,
which keep the inode's block usage consistent with the filesystem's mount
limits.

Page Cache Integration
======================

tmpfs uses the kernel's page cache (xarray) to index its pages by file
offset.  When a page is read or faulted in, the page cache is checked
first.  If the page has been swapped out, a swap entry is found in its
place, and the page is swapped back in transparently.

When a page is added to the cache for a tmpfs file, it replaces any
existing swap entry at that offset.  When a page is evicted by reclaim,
a swap entry takes its place.  Shadow entries (see
Documentation/mm/page_reclaim.rst) may also be stored to support working
set detection.

Swap Integration
================

Under memory pressure, the reclaim path can evict tmpfs pages to swap just
like anonymous pages.  This is transparent to the filesystem — the page
cache slot simply transitions from holding a folio to holding a swap entry.

When a process accesses a swapped-out tmpfs page, the page fault handler
reads the swap entry from the page cache, allocates a new page, reads the
data from swap, and inserts the page back into the cache.  This swap-in
path is specific to shmem and handles locking between concurrent faults
on the same page.

Huge Page Support
=================

tmpfs can allocate transparent huge pages for its files.  The ``huge=``
mount option controls the policy:

- ``never``: only base pages (default).
- ``always``: attempt huge page allocation for every new page.
- ``within_size``: use huge pages only within the file's current size.
- ``advise``: use huge pages only for mappings with ``MADV_HUGEPAGE``.

When a huge page is allocated but only partially used (e.g., a file is
smaller than a huge page), memory is wasted.  To mitigate this, tmpfs
registers a shrinker that identifies huge pages where the file has been
truncated or punched below the huge page boundary, and splits them back
into base pages so the unused portion can be reclaimed.

Accounting and Limits
=====================

Mount Options
-------------

tmpfs mounts accept ``size=`` and ``nr_inodes=`` options that cap the
total blocks and inodes in the filesystem.  Every page allocation is
checked against the block limit; if the limit would be exceeded, the
allocation fails with ``ENOSPC``.

These limits are enforced in-kernel and apply to all users of the
filesystem.  They can be changed at remount time.

Quota Support
-------------

With ``CONFIG_TMPFS_QUOTA``, tmpfs supports user and group quotas.  Each
allocated block is charged to the owning user/group, and allocations fail
if the quota is exceeded.  Quota state is stored in memory and does not
persist across mounts.

Memory Cgroups
--------------

tmpfs pages are charged to the memory cgroup of the process that
instantiates them.  This means tmpfs memory counts toward cgroup limits
and can trigger cgroup-level reclaim.  Swapping a tmpfs page out and back
in preserves its cgroup association.

fallocate
=========

tmpfs supports ``fallocate()`` to preallocate space for a file.
Preallocated pages are allocated and inserted into the page cache
immediately, guaranteeing that subsequent writes will not fail with
``ENOSPC``.

``FALLOC_FL_PUNCH_HOLE`` is also supported: it removes pages from a range
of the file and returns them to the filesystem's free pool.  This is used
by applications that want to release portions of a tmpfs file without
truncating it.
