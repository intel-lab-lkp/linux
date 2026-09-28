.. SPDX-License-Identifier: GPL-2.0

====================
Crash memory actions
====================

`CONFIG_CRASH_MEMACTION` provides a mechanism for the running kernel to
communicate a single bit attribute per memory page to a kdump kernel. This
feature can be used by the kdump kernel to exclude certain pages (e.g. holding
crypto secrets, or cache) from the system memory dump, or to wipe their contents
after a  crash.

The meaning of the registry bitmap is set by the ``crash_memaction=`` kernel
cmdline parameter. The running kernel only sets the bits for the tracked pages,
and it's up to the kdump kernel to do something with them.

	``crash_memaction={secret|cache}``

Right now, ``secret`` tracks pages containing kernel crypto keys as well as
pages explicitly marked using ``madvise(2)``. ``cache`` currently only tracks
pages marked from userspace using ``madvise(2)``.

Kernel users
============

Kernel code can mark a virtual range in the linear map or in vmalloc space::

	void crash_memaction_mark(void *addr, size_t size, int types);
	void crash_memaction_unmark(void *addr, size_t size);

Marking is safe from any context and cannot fail. Markings last until the page
frame is handed out to another user to cover stale data left over after the page
is free'd.

For objects smaller than a page, lib/secret_pool provides allocations from a
"secret" marked kmem_buckets. Objects can be allocated through secret_pool
without the need for any additional lifetime/marking tracking.

Userspace interface
===================

For userspace code, ``MADV_CRASH_SECRET``, ``MADV_CRASH_CACHE`` and
``MADV_CRASH_RESET`` are provided for use with ``madvise(2)``. Marks show up in
``/proc/pid/smaps`` as ``cm`` VMA flag. Only anonymous, shmem and hugetlb ranges
can be marked. Marking is transparent to swapping and lazy allocation.

Limitations
===========

* This mechanism is best-effort. During a crash, nothing can be guaranteed. At
  page level granularity, some over- or under-marking is to be expected.
* Marks are tracked only for *mapped* folios, so a shmem file only ever written
  with ``write(2)`` cannot be covered.
* A single ``madvise(2)`` call on a folio mapped into several processes globally
  (un)marks it for all of them.
* Marks are tracked at page level granularity. There is no refcounting. When
  trying to mark smaller objects, use lib/secret_pool or a similar mechanism.
* Memory hotplug is not supported at this time. When memory is hotplugged, the
  newly hotplugged memory will not be included in the bitmap.

Inspecting the bitmap
=====================

With ``CONFIG_CRASH_MEMACTION_DEBUGFS`` the bitmap can be read through debugfs::

	# types 0x3000 page_shift 12 nr_regions 1
	# start_pfn nr_pages offset bytes paddr
	0x0000000000000001 0x0000000000800000 0x0 0x100000 0x0000000040000000
