.. SPDX-License-Identifier: GPL-2.0

==============================
Allocating dma-buf using heaps
==============================

Dma-buf Heaps are a way for userspace to allocate dma-buf objects. They are
typically used to allocate buffers from a specific allocation pool, or to share
buffers across frameworks.

Heaps
=====

A heap represents a specific allocator. The Linux kernel currently supports the
following heaps:

 - The ``system`` heap allocates virtually contiguous, cacheable, buffers.

 - The ``cma`` heap allocates physically contiguous, cacheable,
   buffers. Only present if a CMA region is present. Such a region is
   usually created either through the kernel commandline through the
   `cma` parameter, a memory region Device-Tree node with the
   `linux,cma-default` property set, or through the `CMA_SIZE_MBYTES` or
   `CMA_SIZE_PERCENTAGE` Kconfig options. Depending on the platform, it
   might be called ``reserved``, ``linux,cma``, or ``default-pool``.

Naming Convention
=================

A good heap name is a name that:

- Is stable, and won't change from one version to the other;

- Describes the memory region the heap will allocate from, and will
  uniquely identify it in a given platform;

- Doesn't use implementation details, such as the allocator;

- Can describe intended usage.

For example, assuming a platform with a reserved memory region located
at the RAM address 0x42000000, intended to allocate video framebuffers,
and backed by the CMA kernel allocator. Good names would be
`memory@42000000` or `video@42000000`, but `cma-video` wouldn't.
