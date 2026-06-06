.. SPDX-License-Identifier: GPL-2.0

==================================
Slab Cache Isolation for Security
==================================

Overview
========

The SLUB allocator merges slab caches with compatible size, alignment, and
flags to reduce memory fragmentation. While this improves memory efficiency,
it allows objects of different types to share the same slab pages. This
enables cross-cache heap exploitation, where a use-after-free in one object
type can be leveraged to corrupt an unrelated type.

The `SLAB_NO_MERGE` flag prevents a cache from being merged, ensuring it
receives dedicated slab pages.

When to use SLAB_NO_MERGE
==========================

`SLAB_NO_MERGE` should be considered for slab caches that meet the
following criteria:

1. *Security-critical contents*: The object holds data whose corruption
   leads directly to privilege escalation or security bypass, such as
   credentials, cryptographic keys, or capability sets.

2. *Actually mergeable*: The cache must not already be unmergeable.
   A cache is already unmergeable if any of the following is true:

   - It has a constructor (`ctor` argument is non-NULL).
   - It has a non-zero `usersize` (with `CONFIG_HARDENED_USERCOPY`).
   - It already has `SLAB_NO_MERGE` or another `SLAB_NEVER_MERGE` flag.

3. *Bounded allocation volume*: The cache has a predictable number of
   active objects, so the memory cost of dedicated slab pages is
   acceptable.

How merging works
=================

When `kmem_cache_create()` is called:

1. If `usersize` is non-zero, the merge path is skipped entirely.

2. Otherwise, `find_mergeable()` in `mm/slab_common.c` searches for a
   compatible existing cache. A merge is prevented if:

   - The `slab_nomerge` boot parameter is set
   - The new cache has a constructor
   - The new cache's flags include `SLAB_NO_MERGE`
   - No existing cache has compatible size and flags

3. If a compatible cache is found, the new cache becomes an alias. Both
   share the same slab pages.

Verifying merge status
======================

To check whether a cache is merged on a running system::

    # Check how many other caches share its pages
    cat /sys/kernel/slab/<cache_name>/aliases

    # aliases > 0 means other types share this cache's pages

The cross-cache attack class
=============================

Cross-cache attacks exploit slab merging to achieve type confusion:

1. Attacker triggers a use-after-free in object type A.
2. Type A's cache is merged with type B (they share slab pages).
3. The freed type A slot is reallocated as type B.
4. Attacker uses the dangling pointer to corrupt type B.
5. Privilege escalation.

CVE-2022-29582 demonstrates this technique: an io_uring use-after-free is
exploited via cross-cache page-level reallocation to achieve root.

`SLAB_NO_MERGE` prevents step 2: dedicated pages mean a freed slot of
one type cannot be reallocated as a different type.

Tradeoffs
=========

*Memory*: Isolated caches may have partially-filled slab pages that
cannot be used by other types. For caches with bounded allocation counts,
this is typically a few extra pages.

*Performance*: Zero impact on `kmem_cache_alloc()` and
`kmem_cache_free()`. The only effect is at boot when the cache is
created.

Relationship to other mitigations
==================================

`CONFIG_RANDOM_KMALLOC_CACHES`
    Creates 16 copies of each `kmalloc` size class and randomly assigns
    allocations among them. Only affects `kmalloc()` users. Does not
    affect named caches created with `kmem_cache_create()`.

`SLAB_TYPESAFE_BY_RCU`
    Delays freeing the slab page by an RCU grace period. Does not delay
    object slot reuse. Does not prevent cross-cache merging. Solves a
    different problem: safe lockless access to freed-and-reallocated
    objects of the same type.

`slab_nomerge` boot parameter
    Disables merging for all caches globally. `SLAB_NO_MERGE` provides
    the same protection selectively for individual caches without the
    global memory cost.
