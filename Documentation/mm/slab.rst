.. SPDX-License-Identifier: GPL-2.0

===============
Slab Allocation
===============

Cache isolation with SLAB_NO_MERGE
===================================

The slab allocator merges caches with compatible size, alignment, and flags
to reduce memory fragmentation. While this improves memory efficiency, it
allows objects of different types to share the same slab. This enables
cross-cache heap exploitation, where a use-after-free in one object type can
be leveraged to corrupt an unrelated type.

SLAB_NO_MERGE prevents a cache from being merged, ensuring it receives a
dedicated slab. A freed slot in an isolated cache can only be reallocated as
the same object type.

When to use SLAB_NO_MERGE
--------------------------

SLAB_NO_MERGE should be considered for caches holding security-critical
objects whose corruption leads directly to privilege escalation, such as
credentials, cryptographic keys, or capability sets.

It is harmless to specify SLAB_NO_MERGE even if the cache is already
unmergeable for other reasons (e.g., it has a constructor or a non-zero
usersize). The flag communicates intent and ensures the cache remains
isolated if those other properties change in the future.

Verifying merge status
-----------------------

To check whether a cache is merged on a running system::

    # Check how many other caches share its slab
    cat /sys/kernel/slab/<cache_name>/aliases

    # aliases > 0 means other types share this cache's slab

Tradeoffs
----------

**Memory**: Isolated caches may have partially-filled slabs that cannot be
used by other types. The overhead is typically a few extra pages.

**Performance**: Zero impact on kmem_cache_alloc() and kmem_cache_free().
The only effect is at boot when the cache is created.

Relationship to other mitigations
----------------------------------

CONFIG_RANDOM_KMALLOC_CACHES creates multiple copies of each kmalloc size
class and randomly assigns allocations among them. It only affects kmalloc()
users and does not affect named caches created with kmem_cache_create().

SLAB_TYPESAFE_BY_RCU delays freeing the slab by an RCU grace period. It
does not delay object slot reuse and does not prevent cross-cache merging.
It solves a different problem: safe lockless access to freed-and-reallocated
objects of the same type.

The slab_nomerge boot parameter disables merging for all caches globally.
SLAB_NO_MERGE provides the same protection selectively for individual caches
without the global memory cost.

Functions and structures
========================

.. kernel-doc:: mm/slab.h
.. kernel-doc:: mm/slub.c
   :internal:
